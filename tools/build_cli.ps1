param(
  [switch]$Run
)

$ErrorActionPreference = 'Stop'

$cmd = @(
  'g++',
  '-std=c++17', '-O2', '-Wall', '-Wextra', '-pedantic',
  '-D_WIN32_WINNT=0x0A00', '-DWINVER=0x0A00',
  'Asset.cpp', 'Portfolio.cpp', 'Yahoo.cpp', 'main.cpp',
  '-o', 'portfolio_cli.exe',
  '-lwinhttp', '-lws2_32'
)

Write-Host ('Building CLI: ' + ($cmd -join ' '))
& $cmd[0] $cmd[1..($cmd.Length-1)]

if ($Run) {
  Write-Host 'Running portfolio_cli.exe...'
  .\portfolio_cli.exe
}
