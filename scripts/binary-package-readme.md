# C Harness v0.11.0 executable package

Run `bin/ah --help` from the extracted directory. Start the terminal client
with `bin/ah tui --workspace /path/to/project`. Add the package's `bin`
directory to your PATH to run `ah` from another directory. Keep
`ah-flow-vm` next to `ah` for JavaScript flows.

The build includes native Responses WebSocket transport, libcurl 8.23.0,
OpenSSL 3.5.9, HTTP/2 and the TUI. Use `--transport websocket` when an
endpoint supports it, or `--transport auto` for the default OpenAI endpoint.

The terminal must have an installed terminfo entry. On macOS, the optional
ChatGPT Keychain adapter uses `python3` on your PATH. Its Python dependencies
are listed in `share/ah/host/packages/host/requirements.txt`. Linux users can
supply a Responses bearer or a private token file as described by
`ah --help-all`.

The source is the v0.11.0 tag with a packaging patch that locates the Python
host adapter relative to the installed executable. The packaging recipe is
available on the `wit/v0.11.0-binaries` branch.
