"""Python transport and byte-counted Project Mu CDC command protocol."""
from __future__ import annotations

import argparse
import base64
import binascii
import codecs
from dataclasses import dataclass
from datetime import datetime
import os
import multiprocessing
from pathlib import Path
import re
import sys
import tempfile
import threading
import time
import zlib

DEPENDENCIES = Path(__file__).with_name('cdc-deps')
if DEPENDENCIES.is_dir():
    sys.path.insert(0, str(DEPENDENCIES))


class CdcError(Exception):
    pass


def serial_module():
    try:
        import serial
        from serial.tools import list_ports
        return serial, list_ports
    except ImportError as error:
        raise CdcError('Install the dependency: python -m pip install pyserial==3.5') from error


def available_ports():
    _, list_ports = serial_module()
    return sorted(list_ports.comports(), key=lambda p: p.device)


def choose_port(explicit=None):
    if explicit:
        return explicit
    ports = available_ports()
    matches = [p for p in ports if p.vid == 0x18D1 and p.pid == 0x4EE8]
    if len(matches) == 1:
        return matches[0].device
    if not matches and len(ports) == 1:
        return ports[0].device
    names = ', '.join(p.device for p in ports) or 'none'
    raise CdcError(f'Choose a port with --port COM20. Available ports: {names}')


def _serial_worker(connection, name, baud):
    serial, _ = serial_module()
    port = serial.Serial(port=None, baudrate=baud, bytesize=serial.EIGHTBITS,
                         parity=serial.PARITY_NONE, stopbits=serial.STOPBITS_ONE,
                         timeout=0.1, write_timeout=20, xonxoff=False,
                         rtscts=False, dsrdtr=False)
    port.port = name
    port.dtr = True
    port.rts = True
    try:
        port.open()
        connection.send((True, None))
        while True:
            action, argument = connection.recv()
            try:
                if action == 'close':
                    port.close(); connection.send((True, None)); return
                if action == 'read': value = port.read(argument)
                elif action == 'write': value = port.write(argument)
                elif action == 'waiting': value = port.in_waiting
                else: raise ValueError('Unknown serial action')
                connection.send((True, value))
            except Exception as error:
                connection.send((False, str(error)))
    except (EOFError, BrokenPipeError):
        pass
    except Exception as error:
        connection.send((False, str(error)))
    finally:
        port.close()
        connection.close()


class PortProcess:
    """A serial handle owned by a child so blocked Windows IO can be stopped."""
    def __init__(self, connection, process, name):
        self.connection = connection
        self.process = process
        self.name = name
        self.lock = threading.Lock()

    def _receive(self, timeout, stage):
        if not self.connection.poll(timeout):
            self._stop()
            raise CdcError(f'{self.name}: {stage} did not finish within {timeout:g}s; '
                           'the serial worker was stopped and its handle released.')
        try:
            ok, value = self.connection.recv()
        except (EOFError, OSError) as error:
            self._stop()
            raise CdcError(f'{self.name}: serial worker exited during {stage}.') from error
        if not ok:
            raise CdcError(f'{self.name}: {stage} failed: {value}')
        return value

    def _call(self, action, argument=None, timeout=4):
        with self.lock:
            try:
                self.connection.send((action, argument))
            except (EOFError, OSError) as error:
                raise CdcError(f'{self.name}: serial worker is closed.') from error
            return self._receive(timeout, action)

    def _stop(self):
        if self.process.is_alive():
            self.process.terminate()
        self.process.join(timeout=2)
        self.connection.close()

    @property
    def in_waiting(self):
        return self._call('waiting')

    def read(self, size):
        return self._call('read', size)

    def write(self, data):
        return self._call('write', data, timeout=25)

    def close(self):
        try:
            if self.process.is_alive():
                self._call('close', timeout=2)
        finally:
            self._stop()


def _connect_process(name, baud, open_timeout, worker):
    context = multiprocessing.get_context('spawn')
    parent, child = context.Pipe()
    process = context.Process(target=worker, args=(child, name, baud), daemon=True)
    process.start()
    child.close()
    port = PortProcess(parent, process, name)
    try:
        port._receive(open_timeout, 'serial setup')
    except Exception:
        port._stop()
        raise
    return port


def open_port(name, baud=3000000, open_timeout=15):
    return _connect_process(name, baud, open_timeout, _serial_worker)


@dataclass
class Response:
    kind: str
    data: bytes
    message: str = ''
    status: int = 0


class CommandClient:
    def __init__(self, port, timeout=120, progress=None, upload_window=1048576):
        if upload_window < 65536 or upload_window > 1048576 or upload_window % 65536:
            raise CdcError('Upload window must be a multiple of 65536, from 65536 to 1048576 bytes.')
        self.port = port
        self.timeout = timeout
        self.progress = progress
        self.pending = bytearray()
        self.upload_window = upload_window
        self.legacy_upload = False

    def _more(self, deadline, stage, size=65536):
        if time.monotonic() >= deadline:
            raise CdcError(f'Timed out while reading {stage}.')
        try:
            # usbser may report cbInQue=0 even with an outstanding USB read.
            # Fixed-size reads still return partial data at the 100 ms timeout;
            # using cbInQue would turn a large reply into one-byte IPC calls.
            data = self.port.read(size)
        except Exception as error:
            raise CdcError(f'CDC read failed while reading {stage}: {error}') from error
        self.pending.extend(data)

    def _line(self, deadline, stage, read_size=65536):
        while True:
            index = self.pending.find(b'\n')
            if index >= 0:
                if index > 1024:
                    raise CdcError('Response header is too long. Use --shell for UEFI Shell.')
                data = bytes(self.pending[:index])
                del self.pending[:index+1]
                return data.removesuffix(b'\r')
            if len(self.pending) > 1024:
                raise CdcError('Response header is too long. Use --shell for UEFI Shell.')
            self._more(deadline, stage, max(1, read_size-len(self.pending)))

    def request(self, command):
        if not command.strip() or '\r' in command or '\n' in command:
            raise CdcError('Pass exactly one nonempty command.')
        try:
            encoded = command.encode('ascii')
        except UnicodeEncodeError as error:
            raise CdcError('CDC commands must be ASCII; use press unicode <number> for a character.') from error
        if len(encoded) > 127:
            raise CdcError('Command exceeds the firmware limit of 127 bytes.')
        deadline = time.monotonic() + self.timeout
        try:
            count = self.port.write(encoded + b'\r')
        except Exception as error:
            raise CdcError(f'CDC write failed: {error}') from error
        if count != len(encoded)+1:
            raise CdcError(f'Incomplete CDC write: {count}/{len(encoded)+1} bytes.')
        header = self._line(deadline, 'response header')
        if header.startswith(b'ERR '):
            raise CdcError(header.decode('ascii', errors='replace'))
        if header.startswith(b'OK '):
            if command.strip().startswith('boot ') and header == b'OK boot accepted':
                # Keep DTR asserted until the ACK has completed and the device
                # launches the image. Returning immediately can cancel it.
                try:
                    result = self._line(deadline, 'boot result')
                except CdcError as error:
                    return Response('text', b'', f'OK boot accepted; return unobserved ({error})')
                if result.startswith(b'ERR '):
                    raise CdcError(result.decode('ascii', errors='replace'))
                if result != b'OK boot returned':
                    raise CdcError('Invalid boot result.')
                return Response('text', b'', result.decode('ascii'))
            return Response('text', b'', header.decode('ascii', errors='replace'))
        match = re.fullmatch(rb'BEGIN (logcat|logcat-last|scrcat|shell) ([0-9]+)( image/bmp base64| status=0x[0-9a-fA-F]+)?', header)
        if not match:
            raise CdcError('Invalid CDC response header. Use command mode outside UEFI Shell.')
        kind = match[1].decode('ascii')
        wanted = command.strip().lower()
        shell_alias = wanted.startswith(('shell ', 'test ', 'haptic ', 'rpmb-read ')) or wanted == 'rpmb-info'
        if kind != ('logcat' if wanted == 'log' else 'shell' if shell_alias else wanted):
            raise CdcError(f'Unexpected reply: {kind} for {wanted}. Reconnect to reset the session.')
        length = int(match[2])
        limit = 180000000 if kind == 'scrcat' else 262144
        suffix = match[3] or b''
        if length > limit or (kind == 'scrcat' and suffix != b' image/bmp base64') or (kind == 'shell' and not suffix.startswith(b' status=0x')) or (kind in ('logcat', 'logcat-last') and suffix):
            raise CdcError('Invalid CDC payload size or encoding.')
        data = bytearray()
        while len(data) < length:
            if not self.pending:
                self._more(deadline, f'{kind} payload ({len(data)}/{length} bytes)')
                continue
            count = min(length-len(data), len(self.pending))
            data.extend(self.pending[:count])
            del self.pending[:count]
            if self.progress:
                self.progress(kind, len(data), length)
        if self._line(deadline, 'payload separator') != b'' or self._line(deadline, 'response footer') != b'END '+match[1]:
            raise CdcError('Invalid CDC response footer; the output file was not saved.')
        if kind == 'scrcat':
            try:
                data = base64.b64decode(data, validate=True)
            except binascii.Error as error:
                raise CdcError('Invalid screenshot base64; the output file was not saved.') from error
            if len(data) < 54 or data[:2] != b'BM' or int.from_bytes(data[2:6], 'little') != len(data):
                raise CdcError('Invalid screenshot BMP; the output file was not saved.')
        status = int(suffix[10:], 16) if kind == 'shell' else 0
        return Response(kind, bytes(data), status=status)

    def upload(self, source, destination):
        """Stream windows over the current session; close/DTR aborts failures."""
        source = Path(source)
        if not destination or any(c in destination for c in '\r\n\x00'):
            raise CdcError('Choose a relative path inside the RAM disk.')
        with source.open('rb') as stream:
            size = os.fstat(stream.fileno()).st_size
            if size > 0xffffffff:
                raise CdcError('The FAT32 file limit is 4294967295 bytes.')
            crc = 0
            while data := stream.read(1024*1024):
                crc = zlib.crc32(data, crc)
            stream.seek(0)
            window = self.upload_window
            legacy = f'put {destination} {size} 0x{crc:08x}'
            modern = f'put2 {destination} {size} 0x{crc:08x} {window}'
            if self.legacy_upload or window == 65536 or len(modern.encode('ascii')) > 127:
                response = self.request(legacy)
                window = 65536
            else:
                try:
                    response = self.request(modern)
                except CdcError as error:
                    # Only this explicit pre-upload rejection establishes that
                    # the old firmware did not start a RAM transaction.
                    if str(error) != 'ERR unknown command; type help':
                        raise
                    self.legacy_upload = True
                    response = self.request(legacy)
                    window = 65536
            ready = f'OK put ready {size} window={window}'
            if response.kind != 'text' or response.message not in (ready, ready+' pad=1024'):
                raise CdcError('Invalid upload READY response; close the session to cancel.')
            padded = response.message.endswith(' pad=1024')
            count = actual_crc = 0
            while count < size:
                data = stream.read(min(window, size-count))
                if not data:
                    raise CdcError('Source file changed during upload; close the session to cancel.')
                wire = data
                if padded and count+len(data)==size:
                    wire += bytes((-len(data)) % 1024)
                at = 0
                while at < len(wire):
                    sent = self.port.write(wire[at:])
                    if sent is None or sent <= 0 or sent > len(wire)-at:
                        raise CdcError('Incomplete upload write; close the session to cancel.')
                    at += sent
                actual_crc = zlib.crc32(data, actual_crc)
                count += len(data)
                expected = (f'OK put done {size} crc=0x{crc:08x}' if count == size
                            else f'OK put progress {count}').encode('ascii')
                # This reply's wire length is known. Read exactly through its
                # CRLF so a short ACK does not wait 100 ms to fill 64 KiB.
                # Keep the established bulk-read path for variable replies.
                header = self._line(time.monotonic()+self.timeout, 'upload acknowledgement',
                                    read_size=len(expected)+2)
                if header.lower() != expected.lower():
                    raise CdcError(f'Upload rejected or progress mismatch: {header!r}')
                if self.progress:
                    self.progress('upload', count, size)
            if size == 0:
                header = self._line(time.monotonic()+self.timeout, 'empty upload commit')
                if header != b'OK put done 0 crc=0x00000000':
                    raise CdcError(f'Empty upload rejected: {header!r}')
            if actual_crc != crc or stream.read(1):
                raise CdcError('Source file changed during upload.')
            return Response('text', b'', f'OK uploaded {destination}: {size} bytes crc=0x{crc:08x}')

    def upload_directory(self, source, destination=''):
        """Upload a file tree in sorted order, preserving relative paths."""
        source = Path(source)
        if not source.is_dir():
            raise CdcError(f'Upload directory does not exist: {source}')
        files = [file for file in sorted(source.rglob('*')) if file.is_file()]
        prefix = destination.replace('\\', '/').strip('/')
        paths = [(file, (prefix+'/' if prefix else '')+file.relative_to(source).as_posix())
                 for file in files]
        # Validate the complete tree before beginning its first RAM mutation.
        for file, relative in paths:
            size = file.stat().st_size
            try:
                command = f'put {relative} {size} 0xffffffff'.encode('ascii')
            except UnicodeEncodeError as error:
                raise CdcError(f'RAM upload paths must be ASCII: {relative}') from error
            if size > 0xffffffff or len(command) > 127 or any(c in relative for c in '\r\n\x00'):
                raise CdcError(f'File exceeds upload path/size limits: {relative}')
        total = 0
        for file, relative in paths:
            reply = self.upload(file, relative)
            total += file.stat().st_size
            print(reply.message, flush=True)
        return Response('text', b'', f'OK uploaded directory: {len(paths)} files, {total} bytes')


def save_response(response, output=None):
    if response.kind == 'text':
        print(response.message)
        return None
    if response.kind == 'shell' and output is None:
        print(response.data.decode('utf-8', errors='replace'), end='')
        print(f'\nShell exit status: 0x{response.status:x}')
        return None
    if output is None:
        suffix = 'bmp' if response.kind == 'scrcat' else 'txt'
        output = Path(f'{response.kind}-{datetime.now():%Y%m%d-%H%M%S-%f}.{suffix}')
    output = Path(output).resolve()
    data = response.data
    if response.kind in ('logcat', 'logcat-last'):
        output = output.with_suffix('.txt')
        # Conversion is only for the computer's file, after complete framing
        # validation. Keep undecodable bytes visible rather than dropping them.
        data = data.decode('utf-8', errors='backslashreplace').replace('\x00', '\\x00').encode('utf-8')
    elif response.kind == 'scrcat':
        output = output.with_suffix('.bmp')
    temporary = None
    try:
        with tempfile.NamedTemporaryFile(dir=output.parent, prefix=output.name+'.', suffix='.part', delete=False) as stream:
            temporary = Path(stream.name)
            stream.write(data)
        os.replace(temporary, output)
    finally:
        if temporary is not None:
            temporary.unlink(missing_ok=True)
    print(f'Saved {response.kind}: {output} ({len(data)} bytes).')
    return output


def progress_reporter():
    last = 0
    def progress(kind, count, total):
        nonlocal last
        now = time.monotonic()
        if now-last >= .25 or count == total:
            print(f'\rReceiving {kind}: {count}/{total} bytes ({count*100/max(total,1):.0f}%)',
                  end='\n' if count == total else '', file=sys.stderr, flush=True)
            last = now
    return progress


def terminal(port, local_echo=False):
    print('UEFI Shell terminal. Ctrl-Z exits. Arrow keys and Backspace are forwarded.')
    decoder = codecs.getincrementaldecoder('utf-8')(errors='replace')
    def receive():
        data = port.read(8192)
        if data:
            print(decoder.decode(data), end='', flush=True)
    if os.name == 'nt':
        import msvcrt
        arrows = {'H':b'\x1b[A','P':b'\x1b[B','M':b'\x1b[C','K':b'\x1b[D','G':b'\x1b[H','O':b'\x1b[F','S':b'\x1b[3~'}
        while True:
            receive()
            if not msvcrt.kbhit():
                continue
            key = msvcrt.getwch()
            if key == '\x1a':
                return
            if key in ('\x00','\xe0'):
                data = arrows.get(msvcrt.getwch(), b'')
            else:
                data = key.encode('utf-8')
            if data:
                port.write(data)
                if local_echo:
                    print('\n' if key == '\r' else '\b \b' if key == '\b' else key, end='', flush=True)
    else:
        import select, termios, tty
        fd = sys.stdin.fileno();previous = termios.tcgetattr(fd)
        try:
            tty.setraw(fd)
            while True:
                receive()
                if select.select([sys.stdin], [], [], 0)[0]:
                    data = os.read(fd, 64)
                    if b'\x1a' in data or not data:
                        return
                    port.write(data)
        finally:
            termios.tcsetattr(fd, termios.TCSADRAIN, previous)


def main(default_command=None):
    parser = argparse.ArgumentParser(description='Python Project Mu CDC console and command client.')
    parser.add_argument('--port', '-p', help='COM20, /dev/ttyACM0, etc.; automatic when unambiguous')
    parser.add_argument('--list', action='store_true', help='list serial ports without opening them')
    parser.add_argument('--baud', type=int, default=3000000, help='CDC line coding (default: 3000000); USB throughput also depends on firmware and host')
    parser.add_argument('--timeout', type=float, default=120, help='command response timeout in seconds')
    parser.add_argument('--open-timeout', type=float, default=15)
    parser.add_argument('--command', '-c', help='help, logcat, scrcat, "press up", "shell echo hello", or "reboot bootloader"')
    parser.add_argument('--output', '-o', type=Path, help='UTF-8 TXT log, decoded BMP screenshot, or Shell output destination')
    parser.add_argument('--shell', action='store_true', help='raw UEFI Shell terminal')
    parser.add_argument('--upload', type=Path, help='upload this local file to the shared RAM disk')
    parser.add_argument('--upload-dir', type=Path, help='upload a directory tree, preserving relative paths')
    parser.add_argument('--upload-window', type=int, default=1048576,
                        help='upload ACK window bytes: multiples of 65536 up to 1048576 (default); old firmware falls back to 65536')
    parser.add_argument('--dest', help='relative RAM disk destination, e.g. EFI/BOOT/BOOTAA64.EFI')
    parser.add_argument('--local-echo', action='store_true', help='echo typed keys in Shell terminal')
    args = parser.parse_args()
    if args.timeout <= 0 or args.open_timeout <= 0:
        parser.error('Timeouts must be positive.')
    if args.shell and args.command is not None:
        parser.error('--shell cannot be combined with --command.')
    if args.upload and (args.shell or args.command is not None or not args.dest):
        parser.error('--upload requires --dest and cannot be combined with --shell/--command.')
    if args.upload_dir and (args.upload or args.shell or args.command is not None):
        parser.error('--upload-dir cannot be combined with --upload/--shell/--command.')
    if args.dest and not (args.upload or args.upload_dir):
        parser.error('--dest requires --upload or --upload-dir.')
    if args.command is None and not args.upload and not args.upload_dir and not args.shell:
        args.command = default_command
    port = None
    try:
        if args.list:
            for p in available_ports():
                print(f'{p.device}: {p.description} [{p.hwid}]')
            return 0
        name = choose_port(args.port)
        print(f'Opening {name}, {args.baud} 8N1, DTR/RTS enabled...', flush=True)
        port = open_port(name, args.baud, args.open_timeout)
        print(f'Connected to {name}.', flush=True)
        if args.shell:
            terminal(port, args.local_echo)
            return 0
        client = CommandClient(port, args.timeout, progress_reporter(), args.upload_window)
        if args.upload:
            save_response(client.upload(args.upload,args.dest))
        elif args.upload_dir:
            save_response(client.upload_directory(args.upload_dir,args.dest or ''))
        elif args.command is not None:
            save_response(client.request(args.command), args.output)
        else:
            print('Commands: help, logcat, shell <command>, test <args>, haptic <args>, rpmb-info/read, ramdisk create/info/hash/release, usb cdc/ramdisk/off.')
            print('Logs are saved as UTF-8 TXT; screenshots are decoded and saved as BMP.')
            print('Type quit to close. For UEFI Shell use --shell.')
            while True:
                try:
                    command = input('cdc> ').strip()
                except EOFError:
                    break
                if command.lower() in ('quit','exit'):
                    break
                if command:
                    save_response(client.request(command), args.output)
        return 0
    except (CdcError, OSError) as error:
        print(f'ERROR: {error}', file=sys.stderr, flush=True)
        return 1
    except KeyboardInterrupt:
        return 130
    finally:
        if port is not None:
            port.close()

