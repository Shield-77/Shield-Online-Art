# makerom 0.18.4

Official project-local Windows tool. No change was made to the global devkitPro installation.

Release: https://github.com/3DSGuy/Project_CTR/releases/tag/makerom-v0.18.4

Archive: https://github.com/3DSGuy/Project_CTR/releases/download/makerom-v0.18.4/makerom-v0.18.4-win_x86_64.zip

Downloaded archive SHA-256: `d743507dc66d4c6f8efcce05ff8f5f47163a60e54e0e0c57fa12ec2f54acdf2f`

Executable SHA-256: `db810fb9c3d41ecaefa8fcbd13140ecafe9b95e6303cd9244c12a2bca70fa938`

The installed makerom 0.19.0 has conflicting `VER_MINOR` definitions in
`version.h` and `user_settings.h`: its parser writes the title minor version
outside the three-element array. Both separate version arguments and `-ver`
produce an incorrect version. 0.18.4 avoids that parser bug.

Source references:
- https://github.com/3DSGuy/Project_CTR/blob/makerom-v0.19.0/makerom/src/user_settings.c
- https://github.com/3DSGuy/Project_CTR/blob/makerom-v0.19.0/makerom/src/version.h
- https://github.com/3DSGuy/Project_CTR/blob/makerom-v0.19.0/makerom/src/user_settings.h
