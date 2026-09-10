# Profligacy

This is a low-level emulation of the hardware in the Korg Prophecy.
* I'm not affiliated with or endorsed by Korg or anyone else.

You bring your own ROM dumps of the original firmware (2 chips), and this thing runs it.

I spent some months probing the NEC V55PI, Hitachi H8/3003, and (3x) TI TMS57002 chips. I got all the inter-chip UART and DSP host interface signals matching in time and content exactly.

For example, the end-to-end signals are like: MIDI -> UART -> DSP host interface -> digital audio.

Demonstrating accuracy of the digital audio is a more complicated story because of the free-running oscillators. I had to do things piece by piece: render some long sustaining oscillator waveforms through the emulator and hardware, line them up by a phase offset. They still won't be sample exact, but the hardware-to-hardware error is consistent with the hardware-to-emulator error.

DSP2 and DSP3 were easier: I could record the inputs and outputs of the hardware chips (4 channels in and 4 channels out), play back the inputs through my emulated chips, and ensure that the outputs matched.

Additionally I could run the original firmware's DSP programs on a standalone DSP board, which let me control the phase of the oscillators. I don't know if this is an easier claim to believe than the in situ logic probing. In reality, I was coming at this from a lot of directions, and eventually I fixed all the discrepancies.

I'm not modelling the post-DAC analog parts at all because the service manual says the frequency response is supposed to be flat to within 1 dB across the audible band. (Annoyingly, I found my hardware unit *isn't* flat, so I might have to replace some 30-y.o. capacitors or something. I'll let you know if it turns out to be the magic analog warmth.)


## Downloads, Firmware, and Installation

First rule of emulation development is that I can't help you get the firmware. This project is for people who own the original hardware and firmware already.

On first launch a folder picker opens. Point it at a directory containing `korgprop/ic12_v17.bin` and `korgprop/ic22_v17.bin` *or* `korgprop.zip` containing those two bin files.

In AU and VST3 hosts, the eleven continuous performance controls are available as DAW
automation parameters under the `Performance:` prefix: Speed, Knobs 1–5, Wheel 1/Pitch,
Wheel 2/Mod, X/Ribbon X, Y/Log-Wheel 3, and Z/Ribbon Pressure. Moving them on the panel
records host automation; playing or editing host automation updates the visible controls.
Their values are saved in projects, while older project states continue to load with the
modeled hardware defaults for controls they did not store.


## Technical stuff

The main chips are an NEC V55PI, a Hitachi H8/3003, and three TI TMS57002s. The H8 was already in MAME. The V55 was not (though the related V20 and V30 were), and even finding a TRM was a bit difficult. I could only find some copies in Japanese at first ([here](https://www.renesas.com/ja/document/mah/v55pitm) and [here](https://www.renesas.com/ja/document/mah/v55pitm-0)), which I couldn't read at all, but which was no problem for my LLM agents. Some months in, a fellow dev told me they had found [an English copy](https://archive.org/details/v55pi_manual) (thanks, Giulio).

I paid someone to trace the schematics into KiCad (thanks, Houston).

### V55PI
The V55 handles key/switch presses, the LCD, MIDI, etc. It was fun getting things printed to the emulated LCD for the first time. It talks to the H8 over a bidirectional UART (weirdly 31.25 kbit/s there and 32 kbit/s back).

### H8/3003
The H8 reads things like program/parameter changes and note events over the UART from the V55PI and turns them into control messages for the DSP host interface.
* For example, if you switch the oscillator or effects type, it will toggle the PLOAD pin and send a new Program over the host interface to dsp1 or dsp3 (there is only 1 program for the filter section, so dsp2 only gets PLOADed at boot).
* If you play notes or change parameters, the H8 sends CLOAD messages corresponding to Coefficients that control triggers, pitches, amplitudes, filter settings, EG + LFO timings/shapes, etc.

### TMS57002
The meat of the project was the DSP chip: the TI TMS57002. There was a stub implementation in MAME, and the author of that helpfully sent me the User's Guide (thanks, Olivier), which I have uploaded [here](https://archive.org/details/tms57002).

That PDF is an optical scan and sometimes the OCR was not 100% reliable. In order to verify the accuracy of my emulation, I bought a couple samples of the chip off eBay. And I made a few PCB assemblies to peek and poke at it with a Raspberry Pi Pico through some level shifting chips.

The first DSP programs I poked into this 30+ year-old chip were simple. Exercising all the instructions. Confirming edge cases (sign extension, overflow, rounding, clamping, etc.) Lots of little details like that in a DSP chip since arithmetic is the main job. A lot of that behavior is dependent on mode/status bits.

Eventually it was accurate enough to run the full programs from the original firmware. A program is up to 256 24-bit instructions long. I observed 12 programs going to DSP1 for all the oscillator sets. Just 1 going to DSP2 for the filters. And 2 going to DSP3 for the effects sets. And 1 other program that runs in between programs that zeroes out DMEM and other state as much as possible (not all the registers, annoyingly). I wish I had found that sooner because I had to cobble something less reliable myself during my standalone DSP testing.

The scariest type of problem I ran into was: the observable outputs are bit-exact for a while, but then diverge. Easy to see how this happens: you might do some 24-bit * 32-bit math with a 52-bit accumulator, but then only output the 24 MSBs. If you have some error only in the LSBs of the accumulator, it might only become visible after millions of frames. (If the chip had a debugging interface, you could read back all the internal state after each step and life would be easy.) I wrote a bunch of adversarial programs designed to probe scary behavior like this. Verified they would be sensitive to small errors only after a million or so frames. And then I ran them for longer than that on hardware and the emulator and made sure they stayed in sync past that point.

## Making it fast

Once I had an accurate emulation, I had to make it faster. The original interpreter ran at about realtime on my M2 chip. Basically no headroom, so there were audio dropouts depending on your buffer size. The 3 DSP chips were taking the bulk of the time, so I added a dynamic recompilation / JIT mode to the DSP core.

It uses about half a core on my Apple M2 chip, and 80% on my intel iMac last time I checked. Probably room to improve.

## Making it pretty

The original panel is quite button-driven. It's a bit nicer in the real hardware when you can use two hands at once, but pecking around with a mouse isn't fun. I started out with a very auto-generated sysex editor kind of UI with hundreds of sliders for all of the exposed parameters. I got sucked into improving this somewhat.

## Supported systems

I can test macOS on ARM and Intel processors (currently not building a release for macOS/x64; let me know if you want it). A couple alpha testers have it running on Windows/x64.

## License

Profligacy's own source is distributed under the GNU Affero General Public License, version 3 only. See [LICENSE](LICENSE) and [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) for the full license and dependency notices.
