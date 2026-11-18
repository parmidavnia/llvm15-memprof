#!/usr/bin/env python3
"""Compile/run the multi-DSO regression using an already-built LLVM toolchain.

Requires PyYAML. This script never builds LLVM itself. Output is retained in the
specified directory (which must not already exist).
"""
import argparse
import os
from pathlib import Path
import subprocess
import yaml


# Keep every small-tool invocation visible and stop at the first failed step.
def run(args, **kwargs):
    print('+', ' '.join(map(str, args)), flush=True)
    return subprocess.run(list(map(str, args)), check=True, **kwargs)


# Compare the raw and indexed record views through the fields persisted in both.
# Display-only symbol names and nondeterministic lifetime measurements are not
# part of this equality check; counts, sizes and full frame identities are.
def allocations(path):
    profile = yaml.safe_load(path.read_text())['MemprofProfile']
    assert profile['Summary']['NumMibInfo'] == 2
    assert profile['Summary']['NumAllocFunctions'] == 2
    result = {}
    for record in profile['Records']:
        for site in record.get('AllocSites', []):
            stack = site['Callstack']
            key = tuple((f['Function'], f['LineOffset'], f['Column'], f['Inline'])
                        for f in stack)
            assert key not in result
            info = site['MemInfoBlock']
            result[key] = (info['AllocCount'], info['TotalSize'])
    assert len(result) == 2
    return profile, result


# This standalone entry point mirrors the lit regression for builds with tests
# disabled. It uses an already-built compiler/runtime and compiles only fixtures.
def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--tool-dir', required=True, type=Path)
    parser.add_argument('--output-dir', required=True, type=Path)
    args = parser.parse_args()
    tools = args.tool_dir.resolve()
    out = args.output_dir.resolve()
    out.mkdir(parents=True, exist_ok=False)
    root = Path(__file__).resolve().parents[2]
    test = root / 'compiler-rt/test/memprof/TestCases/multiple-profiled-dsos.cpp'
    # Reuse lit's split-file fixture so this runner and the integrated test
    # exercise exactly the same four translation units and executable.
    for section in test.read_text().split('//--- ')[1:]:
        name, content = section.split('\n', 1)
        (out / name.strip()).write_text(content)
    clang = tools / 'clang++'
    # Pair the compiler with its own runtime: mixing LLVM versions would test
    # raw-format compatibility rather than this multi-DSO port.
    resource = Path(subprocess.check_output(
        [str(clang), '-print-resource-dir'], text=True).strip())
    runtimes = list(resource.rglob('libclang_rt.memprof*.so'))
    assert len(runtimes) == 1, f'Expected one dynamic MemProf runtime: {runtimes}'
    runtime = runtimes[0]
    # Make LLD and the shared runtime resolve from the requested toolchain,
    # while keeping generated libraries and all artifacts in the fresh directory.
    env = os.environ.copy()
    env['PATH'] = str(tools) + os.pathsep + env.get('PATH', '')
    env['LD_LIBRARY_PATH'] = (str(runtime.parent) + os.pathsep + str(out) +
                              os.pathsep + env.get('LD_LIBRARY_PATH', ''))
    # Debug locations and preserved call frames make source-level stack checks
    # meaningful. -Bsymbolic below ensures both duplicate definitions execute;
    # --build-id locates mappings, and separate-code satisfies text alignment.
    flags = ['-O1', '-g', '-fPIC', '-fmemory-profile',
             '-fno-omit-frame-pointer', '-mno-omit-leaf-frame-pointer',
             '-fno-optimize-sibling-calls']
    for name in 'abcd':
        run([clang, *flags, '-c', out / f'{name}.cpp', '-o', out / f'{name}.o'],
            env=env)
    for lib, objects in [(1, ['a', 'b']), (2, ['c', 'd'])]:
        run([clang, '-fmemory-profile', '-shared-libsan', '-fuse-ld=lld',
             '-shared', '-Wl,--build-id', '-Wl,-z,separate-code', '-Wl,-Bsymbolic',
             *[out / f'{name}.o' for name in objects], '-o', out / f'lib{lib}.so'],
            env=env)
    run([clang, '-O1', out / 'main.cpp', out / 'lib1.so', out / 'lib2.so',
         '-Wl,-rpath,' + str(out), '-o', out / 'main'], env=env)
    # One shared runtime records both DSOs in one process. Preloading initializes
    # that runtime before the otherwise uninstrumented executable uses them.
    runtime_env = env.copy()
    runtime_env['LD_PRELOAD'] = str(runtime)
    runtime_env['MEMPROF_OPTIONS'] = 'log_path=' + str(out / 'memprof.raw')
    run([out / 'main'], env=runtime_env)
    raw = list(out.glob('memprof.raw.*'))
    assert len(raw) == 1, raw
    profdata = tools / 'llvm-profdata'
    # Images explain addresses in the dump; they are not extra profile inputs.
    # Reverse their order once to rule out accidental first-image ownership.
    binaries = ['--profiled-binary', out / 'lib1.so',
                '--profiled-binary', out / 'lib2.so']
    run([profdata, 'show', '--memory', raw[0], *binaries,
         '-o', out / 'raw.yaml'], env=env)
    run([profdata, 'show', '--memory', raw[0], *binaries[2:], *binaries[:2],
         '-o', out / 'reversed.yaml'], env=env)
    assert (out / 'raw.yaml').read_bytes() == (out / 'reversed.yaml').read_bytes()
    # The indexed view must retain the already-resolved stacks and payloads,
    # and must be inspectable without supplying either original library again.
    run([profdata, 'merge', raw[0], *binaries, '-o', out / 'combined.profdata'],
        env=env)
    run([profdata, 'show', '--memory', out / 'combined.profdata',
         '-o', out / 'indexed.yaml'], env=env)
    profile, raw_sites = allocations(out / 'raw.yaml')
    _, indexed_sites = allocations(out / 'indexed.yaml')
    assert raw_sites == indexed_sites
    # Separate GUIDs alone are insufficient: confirm that each allocation is
    # attached to its own caller and its known size/count, not merely renamed.
    leaves = set()
    seen = set()
    for record in profile['Records']:
        for site in record.get('AllocSites', []):
            stack = site['Callstack']
            assert len(stack) == 2, stack
            assert stack[0]['SymbolName'] == 'shared_allocate'
            caller = stack[1]['SymbolName']
            size = {'lib1_entry': 111, 'lib2_entry': 222}[caller]
            assert site['MemInfoBlock']['AllocCount'] == 1
            assert site['MemInfoBlock']['TotalSize'] == size
            leaves.add(stack[0]['Function'])
            seen.add(caller)
    assert len(leaves) == 2
    assert seen == {'lib1_entry', 'lib2_entry'}
    print('PASS: correct DSO stacks, distinct duplicate-name GUIDs, exact counts/'
          'sizes, indexed roundtrip, and reversed binary order.')
    print('Artifacts:', out)


if __name__ == '__main__':
    main()
