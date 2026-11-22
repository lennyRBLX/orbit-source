"""Reproducible patch for the pinned UnRAR source (tools/build-unrar.sh).

Orbit passes UnRAR an archive it has already opened and validated, and
chooses how many threads UnRAR unpacks with. Nothing else changes: UnRAR
still never writes files for Orbit (it runs in test mode and hands the
unpacked data to Orbit's callback).
"""
from pathlib import Path

root = Path(__file__).resolve().parents[1] / '.deps/unrar'

# The UnRAR licence asks for this paragraph in modified sources.
NOTICE = '''/* Modified for Orbit Store by tools/patch-unrar.py.
 *
 * UnRAR source code may be used in any software to handle
 * RAR archives without limitations free of charge, but cannot be
 * used to develop RAR (WinRAR) compatible archiver and to
 * re-create RAR compression algorithm, which is proprietary.
 * Distribution of modified UnRAR source code in separate form
 * or as a part of other software is permitted, provided that
 * full text of this paragraph, starting from "UnRAR source code"
 * words, is included in license, or in documentation if license
 * is not available, and in source code comments of resulting package.
 */
'''


def patch(name, marker, replacement):
    p = root / name
    source = p.read_text(encoding='latin-1')
    if NOTICE not in source:
        source = NOTICE + source
    if replacement not in source:
        assert source.count(marker) == 1, (name, marker)
        source = source.replace(marker, replacement, 1)
    p.chmod(p.stat().st_mode | 0o200)
    p.write_text(source, encoding='latin-1')


# Open the archive Orbit already holds (a duplicate of its descriptor),
# never a path, while OrbitRAROpenArchiveFd() is running.
patch('file.cpp', '#include "rar.hpp"\n',
      '#include "rar.hpp"\nextern thread_local int OrbitArchiveFd;\n')
patch('file.cpp', '  int handle=open(NameA.c_str(),flags);\n',
      '  int handle=OrbitArchiveFd>=0 && !WriteMode && !UpdateMode ? dup(OrbitArchiveFd) : open(NameA.c_str(),flags);\n')

# The thread count Orbit asks for, instead of every CPU the system reports.
patch('threadmisc.cpp', 'uint GetNumberOfThreads()\n{\n',
      'extern thread_local uint OrbitThreads;\nuint GetNumberOfThreads()\n{\n  if (OrbitThreads!=0)\n    return OrbitThreads;\n')

def patch_all(name, marker, replacement):
    p = root / name
    source = p.read_text(encoding='latin-1')
    if NOTICE not in source:
        source = NOTICE + source
    if marker in source:
        source = source.replace(marker, replacement)
    p.chmod(p.stat().st_mode | 0o200)
    p.write_text(source, encoding='latin-1')


# The PS5 SDK has no compiler-rt CPU probe (__cpu_model). The PS5 is always
# the same AMD Zen 2, so its build (ORBIT_PS5_CPU) names the features it has
# that UnRAR uses: SSE to SSE4.1 and AES-NI. AVX2 changes nothing in UnRAR.
patch('system.hpp', 'SSE_VERSION GetSSEVersion();\n',
      '''SSE_VERSION GetSSEVersion();
#ifdef ORBIT_PS5_CPU
#define ORBIT_CPU_SUPPORTS(f) (strcmp(f,"avx2")!=0)
#else
#define ORBIT_CPU_SUPPORTS(f) __builtin_cpu_supports(f)
#endif
''')
patch_all('system.cpp', '__builtin_cpu_supports(', 'ORBIT_CPU_SUPPORTS(')
patch_all('rijndael.cpp', '__builtin_cpu_supports(', 'ORBIT_CPU_SUPPORTS(')

patch('dll.cpp', 'HANDLE PASCAL RAROpenArchive(struct RAROpenArchiveData *r)\n',
      '''thread_local int OrbitArchiveFd=-1;
thread_local uint OrbitThreads=0;

// RAROpenArchiveEx() for an archive Orbit has already opened as fd, unpacking
// with the given number of threads (0: UnRAR's default).
extern "C" HANDLE PASCAL OrbitRAROpenArchiveFd(struct RAROpenArchiveDataEx *r,int fd,unsigned int threads)
{
  OrbitArchiveFd=fd;
  OrbitThreads=threads;
  HANDLE h=RAROpenArchiveEx(r);
  OrbitArchiveFd=-1;
  OrbitThreads=0;
  return h;
}


HANDLE PASCAL RAROpenArchive(struct RAROpenArchiveData *r)
''')
