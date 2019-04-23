:: Copyright 2019 The Chromium Authors. All rights reserved.
:: Use of this source code is governed by a BSD-style license that can be
:: found in the LICENSE file.

:: Executes msbuild after configuring environment with vcvars64.

set VCVARS_PATH=%1
set BUILD_CONFIG=%2
set SOLUTION_PATH=%3

call %VCVARS_PATH%
if %errorlevel% neq 0 exit /b %errorlevel%

msbuild "%SOLUTION_PATH%" /p:Configuration=%BUILD_CONFIG%