# Dragon GL — Quick Start

Copyright (C) 2026 Nicola Taibi. Dragon GL is free software, released under the GNU General Public License version 3 or later (see [LICENSE.txt](LICENSE.txt)).

## 1. Build

```bash
./build.sh -n          # Release build, no tests
```

See [BUILD.md](BUILD.md) for dependencies and every `build.sh` option.

## 2. Start the server

```bash
./dragongl-server --password "anypassword"
```

Add `--dm-password <pwd>` to enable the Dungeon Master account and `--port <n>` to change the TCP port (default 8080).

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
