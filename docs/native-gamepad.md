# Native Wii U GamePad backend

The Linux input prototype receives native 128-byte DRC datagrams on UDP port 50022 and feeds buttons, sticks, and touch into VPAD channel 0. It requires a network on which the GamePad is already associated and sending input. The prototype does not configure the wireless adapter, pair the GamePad, or transmit console-side protocol traffic.

Set `CEMU_DRC_BIND_ADDRESS` to the console-side IPv4 address on the dedicated link and `CEMU_DRC_PEER_ADDRESS` to the GamePad's IPv4 address. Both are required. Configure player 1 as a Wii U GamePad in Cemu. With the receiver enabled, native input expires after 150 ms without a valid packet. Use an isolated GamePad link since the receiver filters by source IP and relies on the network for peer authentication.

## Console interfaces

- The renderer has independent TV and GamePad scanout paths. Native streaming needs a DRC scanout sink that runs even when the desktop pad window is closed.
- AX produces a separate DRC audio mix. VPAD has a motor-pattern queue. A native transport needs to deliver both to the physical pad.
- `mic.rpl` and `camera.rpl` are present in the NAND dump. Cemu needs physical microphone samples in the guest MIC ring buffer and physical camera frames in the guest CAM callback path.
- The dumped `nsysccr.rpl` exports `CCRCDCPerIrdaControl`. Wii Fit U imports it for Fit Meter communication. Supporting the meter requires the GamePad's bidirectional infrared transceiver and a guest implementation of its control path.
- The inspected `h264.rpl` exposes H264DEC entry points. Those exports do not provide the outgoing GamePad video encoder. The outgoing stream and the location of the original encoding path are still under investigation.

[DRC input protocol](https://libdrc.org/docs/re/sc-input.html) · [Nintendo's Fit Meter pairing instructions](https://en-americas-support.nintendo.com/app/answers/detail/a_id/9009/p/172)

## Pending Sync work

The Linux Options menu enables input on an existing GamePad Wi-Fi link. The console's SYNC button is a separate feature. Options → Press Wii U Console SYNC sends an IM_EVENT_SYNC notification to the guest when it has subscribed through IM_GetEventNotify. With the Wii U Menu open, the second press should bring up Nintendo's four-symbol screen on the emulated TV. The same pairing session must accept those symbols when entered on the physical GamePad after pressing its own SYNC button. Verify the Nintendo screen and successful pairing together on hardware.

A VA-API probe on the Radeon produced 864×480 constrained-baseline H.264 with no B-frames and five slices starting at macroblocks 0, 324, 648, 972, and 1296. GamePad decoding still requires verifying the full bitstream contract and transport on hardware.

The Native Wii U GamePad settings should expose Capture Wi-Fi and Release Wi-Fi. A privileged helper should own the selected adapter only for the pairing and runtime session and restore NetworkManager control on release or exit. The UI should obtain the link addresses from that session.

The retail Menu registers for IM event bit 7 and re-registers after each callback. The callback calls WPADStartSyncDevice before the Menu proceeds into its sync UI. Cemu now exposes the guest WPAD sync state and callbacks. Pairing state still needs to be connected to the physical adapter and Nintendo's CCR calls.

Physical console TCP syslog uses the MochaPayload discovery beacon (UDP 4445) followed by an inbound console connection to TCP 7965. During the hardware test the first SYNC press logged Bluetooth pairing loop 3, and the second press was followed by pairing loop 0. The Wii U Menu in Cemu received the virtual IM event and invoked WPADStartSyncDevice. No over-the-air GamePad handshake was captured in that TCP log.
