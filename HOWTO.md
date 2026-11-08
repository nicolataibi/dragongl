# Dragon GL — Quick Start

Copyright (C) 2026 Nicola Taibi. Dragon GL is free software, released under the GNU General Public License version 3 or later (see [LICENSE.txt](LICENSE.txt)).

## 1. Build

```bash
./build.sh -n          # Release build, no tests
```

See [BUILD.md](BUILD.md) for dependencies and every `build.sh` option.

## 2. Start the server

```bash
./dragongl-server                              # asks for the passwords
./dragongl-server --password-file server.pw    # first line of the file (chmod 600)
```

Passwords: 8-63 printable ASCII characters, no spaces; they are never accepted on the command line. A Dungeon Master password (asked at start-up, or `--dm-password-file dm.pw`) enables the `dm` account. `--port <n>` changes the TCP port (default 8080).

## 3. Start the client

```bash
./dragongl-client gl                 # OpenGL backend
./dragongl-client vk                 # Vulkan backend
./dragongl-client vk 192.168.1.10    # remote server, default port
```

## 4. Reset the world

```bash
./dragongl-reset-world
```

Deletes player saves, the generated world and the logs: the next server start generates a new world.
