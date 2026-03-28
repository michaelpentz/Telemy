# Third-Party Notices

Telemy incorporates and interacts with the following third-party components:

## SRT Relay (srtla)

- **Repository:** [michaelpentz/srtla](https://github.com/michaelpentz/srtla)
- **License:** GNU Affero General Public License v3.0 (AGPL-3.0)
- **Original Author:** BELABOX project
- **Additional Copyright:** IRLToolkit Inc., OpenIRL
- **Relationship:** Separate process. Telemy communicates with srtla over the network (SRT protocol). The srtla source code is available at the repository linked above.

## SRT Relay Receiver (srtla-receiver)

- **Repository:** [michaelpentz/srtla-receiver](https://github.com/michaelpentz/srtla-receiver)
- **License:** GNU General Public License v3.0 (GPL-3.0)
- **Original Author:** OpenIRL
- **Relationship:** Docker image used for relay infrastructure. Source code is available at the repository linked above.

## Go Dependencies

All Go module dependencies use permissive licenses (MIT, BSD, Apache-2.0).
See `control-plane/go.mod` for the full dependency list.

## OBS Studio SDK

- **License:** GNU General Public License v2.0 (GPL-2.0)
- **Relationship:** Telemy links against the OBS Studio plugin API as a dynamically loaded module. The OBS plugin DLL is distributed separately.

---

*This notice is provided for informational purposes. Each component retains
its original license terms. See individual repositories for full license text.*
