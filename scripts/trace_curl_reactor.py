#!/usr/bin/env python3
"""Temporary deep tracing overlay for the a880483 evidence generator.

Original source bytes are restored in finally. Tracing is never installed in
the committed production sources; the next ordinary build removes it.
"""

import argparse
from contextlib import contextmanager
import json
import os
from pathlib import Path
import subprocess

from compare_curl_reactor_evidence import read_evidence
from investigate_curl_reactor import diagnostic_statistics, prepare_binaries

PREFIX = 'a2a::http::stage_trace::'


def emit(kind, key='nullptr', argument='0'):
    return f'{PREFIX}Emit({PREFIX}Kind::k{kind}, {key}, {argument});'


def span(variable, begin, end, key, argument='0'):
    return (f'const {PREFIX}Span {variable}({PREFIX}Kind::k{begin}, '
            f'{PREFIX}Kind::k{end}, {key}, {argument});')


def replace(text, before, after, count=1):
    if text.count(before) != count:
        raise ValueError(f'overlay anchor changed: {before!r}')
    return text.replace(before, after)


def instrument(path, text, header):
    if path.name == 'curl_stream_reactor.cpp':
        for function, statement in (
            ('Enqueue(Command command)', emit('Enqueue', 'command.transfer ? command.transfer->easy_handle : nullptr',
                                               'static_cast<std::int64_t>(command.type)')),
            ('ProcessCommands()', span('commands_span', 'CommandsBegin', 'CommandsEnd', 'this')),
            ('AddOnReactor(std::shared_ptr<Transfer> transfer)', span('add_span', 'AddBegin', 'AddEnd', 'transfer->easy_handle')),
            ('ProcessWindowsSocket(curl_socket_t socket, short events)', span('action_span', 'ActionBegin', 'ActionEnd', 'this',
                                                                           'static_cast<std::int64_t>(socket)')),
        ):
            anchor = f'void CurlStreamReactor::{function} {{'
            text = replace(text, anchor, anchor + '\n  ' + statement)
        anchor = '  const CURLMcode action_code = curl_multi_socket_action'
        text = replace(text, anchor, '  ' + span('action_span', 'ActionBegin', 'ActionEnd', 'this', 'static_cast<std::int64_t>(CURL_SOCKET_TIMEOUT)') + '\n' + anchor)
        anchor = '  const CURLMcode poll_code = curl_multi_poll'
        text = replace(text, anchor, '  ' + emit('WaitBegin', 'this', 'bounded_timeout') + '\n' + anchor)
        anchor = '  if (poll_code != CURLM_OK) {'
        text = replace(text, anchor, '  ' + emit('WaitEnd', 'this', 'ready') + '\n' + anchor)
        anchor = '  if (curl_multi_perform(multi_handle_, &running_handles) != CURLM_OK) {'
        text = replace(text, anchor, '  ' + span('action_span', 'ActionBegin', 'ActionEnd', 'this') + '\n' + anchor)
        anchor = '    if (windows_readiness_->IsCurrent(event)) {\n      ProcessWindowsSocket(event.socket, event.events);\n    }'
        text = replace(text, anchor, anchor + ' else {\n      ' + emit('Stale', 'this', 'static_cast<std::int64_t>(event.socket)') + '\n    }')
    elif path.name == 'curl_windows_readiness.cpp':
        for function, statement in (
            ('Wake() const noexcept', emit('Wake', 'this')),
            ('DrainWake() const noexcept', span('drain_span', 'DrainBegin', 'DrainEnd', 'this')),
            ('Update(SOCKET socket, int curl_action)', emit('Interest', 'this', 'static_cast<std::int64_t>(socket)')),
        ):
            return_type = 'void' if function.startswith('Wake') else 'bool'
            anchor = f'{return_type} CurlWindowsReadiness::{function} {{'
            text = replace(text, anchor, anchor + '\n  ' + statement)
        anchor = '    result = WSAPoll(sockets_.data(), static_cast<ULONG>(sockets_.size()), deadline_.WaitMilliseconds());'
        text = replace(text, anchor, '    ' + emit('WaitBegin', 'this', 'deadline_.WaitMilliseconds()') + '\n' + anchor +
                       '\n    ' + emit('WaitEnd', 'this', 'result'))
    elif path.name == 'http_client_streaming.cpp':
        for method, kind in (('QueueMetadata(Response response)', 'Metadata'), ('QueueChunk(std::string chunk)', 'Body')):
            anchor = f'  void {method} {{'
            text = replace(text, anchor, anchor + '\n    ' + emit('Queue' + kind, 'slot->easy_handle'))
        anchor = '          const auto handled = self->metadata_handler(response);'
        text = replace(text, anchor, '          ' + emit('DispatchMetadata', 'self->slot->easy_handle') + '\n' + anchor)
        anchor = '              const auto handled = self->chunk_handler(chunk);'
        text = replace(text, anchor, '              ' + emit('DispatchBody', 'self->slot->easy_handle') + '\n' + anchor)
        anchor = '  async_state->transfer = std::make_shared<detail::CurlStreamReactor::Transfer>();'
        setup = (f'  (void)curl_easy_setopt(handle, CURLOPT_DEBUGFUNCTION, {PREFIX}CurlDebug);\n'
                 '  (void)curl_easy_setopt(handle, CURLOPT_VERBOSE, 1L);\n')
        text = replace(text, anchor, setup + anchor)
    elif path.name == 'curl_reactor_evidence.cpp':
        probe = header.with_name('curl_reactor_connect_probe.h')
        text = f'#include "{probe.as_posix()}"\n' + text
        anchor = 'int main(int argc, char** argv) {\n  try {'
        text = replace(text, anchor, anchor + f'\n    const {PREFIX}ConnectProbe connect_probe;')
        anchor = '      diagnostic->origin = CurlStreamProbe::Clock::now();'
        text = replace(text, anchor, anchor + f'\n      {PREFIX}EmitAt(diagnostic->origin, {PREFIX}Kind::kOrigin, count, '
                       'static_cast<std::int64_t>(diagnostic->repetition));')
        anchor = '    WriteCurlDiagnostics(options, kEvidenceTimerPeriodMs, diagnostics);'
        text = replace(text, anchor, anchor + f'\n    {PREFIX}Dump();')
    else:
        raise ValueError(f'unexpected overlay source: {path}')
    return f'#include "{header.as_posix()}"\n' + text


@contextmanager
def source_overlay(repository):
    header = repository / 'tests/support/curl_reactor_stage_trace.h'
    paths = [repository / name for name in (
        'src/http/curl_stream_reactor.cpp', 'src/http/curl_windows_readiness.cpp',
        'src/http/http_client_streaming.cpp', 'tests/curl_reactor_evidence.cpp')]
    original = {path: path.read_bytes() for path in paths}
    patched = {path: instrument(path, data.decode().replace('\r\n', '\n'), header).encode()
               for path, data in original.items()}
    try:
        for path, data in patched.items():
            path.write_bytes(data)
        yield
    finally:
        conflicts = []
        for path, data in original.items():
            current = path.read_bytes()
            if current == patched[path]:
                path.write_bytes(data)
            elif current != data:
                conflicts.append(str(path))
        if conflicts:
            raise RuntimeError(f'concurrent source edit; refusing restoration: {conflicts}')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build-dir', type=Path, required=True)
    parser.add_argument('--configuration', default='RelWithDebInfo')
    parser.add_argument('--output-dir', type=Path, required=True)
    args = parser.parse_args()
    repository = Path(__file__).resolve().parents[1]
    output = args.output_dir.resolve()
    output.mkdir(parents=True, exist_ok=True)
    manifest = {'commands': []}
    with source_overlay(repository):
        binaries, manifest['build_commands'] = prepare_binaries(args.build_dir.resolve(), args.configuration)
        for order_name, order in (('native-first', ('native', 'fallback')), ('fallback-first', ('fallback', 'native'))):
            variants = ('original-yield', 'skip-yield') if order_name == 'native-first' else ('skip-yield', 'original-yield')
            for variant in variants:
                for name in order:
                    directory = output / order_name / variant
                    directory.mkdir(parents=True, exist_ok=True)
                    environment = os.environ.copy()
                    environment['A2A_STAGE_TRACE_FILE'] = str(directory / f'{name}-stages.csv')
                    environment['A2A_STAGE_SKIP_CONNECT_YIELD'] = '1' if variant == 'skip-yield' else '0'
                    command = [str(binaries[name]), '--diagnostics', str(directory / f'{name}-timing.json'),
                               '--complete-only', '--repetitions', '5', '--reuse-client', '--warmup']
                    manifest['commands'].append(dict(order=order_name, variant=variant, backend=name, command=command))
                    print(f'Tracing {order_name}/{variant}/{name}', flush=True)
                    result = subprocess.run(command, env=environment, capture_output=True, text=True, check=False)
                    (directory / f'{name}.json').write_text(result.stdout, encoding='utf-8')
                    (directory / f'{name}.stderr').write_text(result.stderr, encoding='utf-8')
                    (output / 'manifest.json').write_text(json.dumps(manifest, indent=2), encoding='utf-8')
                    if result.returncode:
                        raise RuntimeError(f'trace run failed ({result.returncode}): {result.stderr}')
                    evidence = read_evidence(directory / f'{name}.json', ('complete',))
                    timing = json.loads((directory / f'{name}-timing.json').read_text(encoding='utf-8-sig'))
                    diagnostic_statistics(timing, evidence)


if __name__ == '__main__':
    main()
