$ErrorActionPreference = 'Stop'

$cmd = @(
  'g++',
  '-std=c++17', '-O2', '-Wall', '-Wextra', '-pedantic',
  '-I.',
  'tests/asset_portfolio_tests.cpp', 'Asset.cpp', 'Portfolio.cpp',
  '-o', 'asset_portfolio_tests.exe'
)

Write-Host ('Building tests: ' + ($cmd -join ' '))
& $cmd[0] $cmd[1..($cmd.Length-1)]

Write-Host 'Running tests...'
.\asset_portfolio_tests.exe
