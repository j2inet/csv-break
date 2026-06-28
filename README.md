# csv-break

A Win32/C++ console application that splits a large CSV file into multiple smaller
files. It uses ANSI escape sequences for coloured console output and `VirtualAlloc`
for memory management.

## Building

Open **csv-break.sln** in Visual Studio 2022 (or later) and build the desired
configuration (`Debug|x64` is a good starting point).

Alternatively, build from the Developer Command Prompt:

```cmd
msbuild csv-break.sln /p:Configuration=Release /p:Platform=x64
```

## Usage

```
csv-break.exe [options] <input-file>

Options:
  -l <max-lines>    Maximum number of data lines per output file
  -s <max-size>     Maximum output file size (bytes; append K, M, or G for
                    larger units, e.g. 10M for 10 megabytes)
  -p <prefix>       Output file name prefix (default: output)
  -r                Replicate the header row in every output file
  -?                Show help
```

At least one of **-l** or **-s** must be supplied; both may be specified together
(a new file is opened as soon as either limit is reached).

Output files are named `<prefix>NNNN.csv`, where NNNN is the file sequence
number zero-padded to four digits (e.g. `output0001.csv`, `output0002.csv`, …).

## Examples

Split `big.csv` into files of at most 10 000 data lines, replicating the
header row in each output file and using the prefix `chunk_`:

```cmd
csv-break.exe -l 10000 -r -p chunk_ big.csv
```

Split `big.csv` into files no larger than 5 MB each:

```cmd
csv-break.exe -s 5M big.csv
```

## Memory management

The program calls `GlobalMemoryStatusEx` to check how much physical RAM is
available. If there is enough free RAM to hold the entire input file (plus a
64 MB safety margin), `VirtualAlloc` is used to allocate a single buffer for
the whole file and it is read in one pass. Otherwise a smaller rolling buffer
is allocated (at most half of available RAM, capped at 256 MB) and the file is
processed in chunks while preserving line boundaries.