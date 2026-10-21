# SPDX-License-Identifier: MPL-2.0
$ErrorActionPreference = "Stop"
python (Join-Path $PSScriptRoot "aster.py") @args
exit $LASTEXITCODE
