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

Connections are encrypted (TLS 1.3). At the first start the server creates its certificate in `state/tls/` and logs its fingerprint (`TLS fingerprint: SHA256:...`): give it to the players. `--tls-cert <file> --tls-key <file>` use a certificate of your own.

## 3. Start the client

```bash
./dragongl-client                    # asks for the server host
./dragongl-client 192.168.1.10       # remote server, default port
./dragongl-client 2001:db8::10 9000  # IPv6 address and port
./dragongl-client --fingerprint SHA256:0E:0D:...:C9 game.example.org   # pin the certificate
```

At the first connection to a server the client prints the server's fingerprint: compare it with the one the server owner gave you. The client records it in `~/.config/dragongl/known_servers` and refuses the server if its certificate changes later (remove its line there if the owner confirms the change).

## 4. Reset the world

```bash
./dragongl-reset-world
```

Deletes player saves, the generated world and the logs: the next server start generates a new world.
