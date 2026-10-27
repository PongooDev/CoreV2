<h1>
  <img src="https://github.com/user-attachments/assets/28d73a41-1c02-4343-8365-3a19d0771d48" width="50" height="50" align="absmiddle" />
  &nbsp;Core V2
</h1>

**Core V2** is a community made Fortnite gameserver focused on recreating original behavior as accurately as possible, starting from the earliest x64 Fortnite builds and moving forward one build at a time. It is a full rewrite of [Core](https://github.com/PongooDev/Core), built to be more accurate and more maintainable across many builds.

Core runs inside Fortnite to provide server-side systems to load maps, accept players, run matches, and support gameplay. This repository does NOT include Fortnite game files or a launcher. The backend is a separate project: [Core-Backend](https://github.com/PongooDev/Core-Backend).

> Core is an independent community project. It is not affiliated with, endorsed by, or supported by Epic Games.

**Please refer to the [Contributing](#contributing) section before posting an issue, Thank you!**

[Join the Core Discord](https://discord.gg/auzEaKs7AS) · [Report an issue](https://github.com/PongooDev/CoreV2/issues)

## 📥[Download](https://github.com/PongooDev/CoreV2/releases)📥

Looking for the original Core? Core V1 is still available at [PongooDev/Core](https://github.com/PongooDev/Core).

## Version support

| Status | Fortnite versions |
| --- | --- |
| Full support ✅ | 1.1 – 1.2 |
| In development 🚧 | 1.7.2 – 1.8 |

Core moves forward one build at a time and skips builds nobody has a copy of, so full support is always one range: every build from the first version to the last one listed.

<details>
<summary>Every build by changelist</summary>

| Fortnite version | Build | Status |
| --- | --- | --- |
| 1.1 | Cert CL-3532353 | Full support ✅ |
| 1.2 | Release-Cert CL-3541083 | Full support ✅ |
| 1.7.2 | Release-Cert CL-3700114 | In development 🚧 |
| 1.8 | Release-Live CL-3724489 | In development 🚧 |

</details>

- **Full support** means that a version is actively supported. We prioritize fixes and missing functionality for fully supported versions before adding support for other versions.
- **In development** means work on the version has started, but it is not fully working yet.
- **Unsupported** means the build may start and have some gameplay features, but it may be unfinished, untested, or have game-breaking bugs.
- When work begins on a new Fortnite version, we aim to make it fully functional before moving on to another version.

## Contents

- [Building](#building)
- [Contributing](#contributing)
- [Credits](#credits)
- [License](#license)

## Building

### Requirements
- Windows 10 or Windows 11
- Visual Studio 2022 or 2026
- Desktop development with C++ workload
- MSVC v143 C++ x64/x86 build tools
- Windows 10 SDK

### Build steps
1. Clone the repository:

   ```
   git clone --recursive https://github.com/PongooDev/CoreV2

   cd CoreV2
   ```

2. Open [`Core.sln`](Core.sln) in Visual Studio
3. Select **Release** and **x64** in the solution toolbar.
4. Build it

The build writes three DLLs to `x64\Release`: `Core.dll` (the server), `CoreClient.dll` (the client) and `CoreRedirect.dll` (points the game's HTTP traffic at Core-Backend).

## Contributing

Contributions are welcome, whether it is code or an issue report.

### Issue reports

Include all of the following in a report if possible:

1. Fortnite version and CL
2. Playlist
3. Number of players
4. Reproduction steps
5. Screenshots
6. Server log
7. Core commit
8. Anything else that may help

**Issues that are overly broad, contain only a crash log without context, or provide too little information to accurately show or reproduce the problem may be closed until the required details are supplied.**

### Code

Before submitting code:

1. Keep unrelated fixes in separate pull requests
2. Keep your code clean and readable. You may create helper functions if needed for better readability.
3. Test your code and include the versions you tested it on.
4. Necessary or helpful logging is encouraged however remove excessive or temporary logging from the final change.
5. Avoid broad version gates unless the compatibility boundary is understood and supported by testing or other evidence.

**Pull requests may be declined when they are inaccurate, untested, overly broad, unrelated to the reported issue, or harmful to other supported versions.**

## Credits

- **PongooDev** — project creator and primary maintainer
- [Core contributors](https://github.com/PongooDev/CoreV2/graphs/contributors)

For more help, join the [Core Discord](https://discord.gg/auzEaKs7AS).

## License

Core V2 is licensed under the [GNU General Public License v3.0](LICENSE).

---
If you use Core in another project, please credit the Core project.
