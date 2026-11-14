# Custom GPU driver loader dependencies

CMake fetches source only from bylaws/libadrenotools at
`8fae8ce254dfc1344527e05301e43f37dea2df80`, including bylaws/liblinkernsbypass
at `aa3975893d83ef1bc84c321ec60c65fbf1287887`. Both revisions are checked
at configure time. No vendor driver binaries are included.

Both projects are BSD-2-Clause, copyright (c) 2021 Billy Laws. The unmodified
upstream license texts beside this file are packaged as APK assets by Gradle.

Only the lazily loaded `libwt_gpu_driver.so` links adrenotools. The ordinary
bootstrap must never link it: linkernsbypass runs invasive linker inspection
in a constructor. Java must durably arm the pending marker before loading it.
Upstream may silently use the system driver; selection is not proof of the
effective GPU driver.
