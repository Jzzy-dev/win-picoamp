# Publishing PicoAmp in the Microsoft Store

An app from the Microsoft Store is signed by Microsoft, so Windows (and
SmartScreen) trusts it. Registering as an individual developer is free.
Every build of this repository makes an MSIX package ready for the Store;
these are the steps to publish it.

## Who publishes

Anyone may publish PicoAmp under its licence (CC BY-NC-SA 4.0): for free, with
credit to JZZY & Claude and to the PicoVerse project, and under the same
licence. Before you start, ask JZZY (and, for the engine's code, the PicoVerse
author) and say so in the listing, so that people know where the app comes from.

Know this before you begin:
* The publisher's name shows in the Store, and only the publisher can send
  updates. If the publisher stops, a new listing has to be made, and its users
  have to install the new one.
* No price, no advertising and no in-app purchases: the licence is
  non-commercial.

## 1. A developer account (free, once)

1. Go to [storedeveloper.microsoft.com](https://storedeveloper.microsoft.com),
   choose **Get started for free**, then **Individual developer**.
2. Sign in with a Microsoft account. Verify your identity with an ID and a selfie.
3. Open the Partner Center dashboard.

## 2. Reserve the name

1. In Partner Center: **Apps and games**, then **New product**, then **MSIX or PWA app**.
2. Reserve a name, for example "PicoAmp" (or "PicoAmp for Windows" if that is taken).
3. Open **Product management**, then **Product identity**. Note three values:
   * `Package/Identity/Name` (like `12345YourName.PicoAmp`)
   * `Package/Identity/Publisher` (like `CN=1A2B3C4D-...`)
   * `Package/Properties/PublisherDisplayName` (your publisher name)

## 3. Build the package with your identity

**On GitHub (the easy way).** This works in this repository or in a fork.
1. Open **Settings**, then **Secrets and variables**, then **Actions**, then **Variables**.
2. Add three repository variables:
   * `MSIX_NAME`: the `Package/Identity/Name`
   * `MSIX_PUBLISHER`: the `Package/Identity/Publisher`
   * `MSIX_PUBLISHER_NAME`: the `PublisherDisplayName`
3. Push a tag such as `v0.2.0`.

The release then has `PicoAmp-0.2.0.0.msix` with your identity. Builds that
are not tagged are numbered `0.2.<run>.0`.

**On your own PC.** You need the Windows SDK, and `make` from MSYS2 to build the exe:

```powershell
pwsh tools/make_msix.ps1 -Version 0.2.0.0 -Name "<Identity Name>" -Publisher "<Identity Publisher>" -PublisherName "<Publisher display name>"
```

Leave the package unsigned: the Store signs it.

To try the package on your own PC first, add `-TestSign` (in an administrator
PowerShell). This signs it with a test certificate and installs it.

## 4. The submission

1. **Pricing and availability:** choose Free and the markets.
2. **Properties:** category *Music*; the privacy policy below.
3. **Age ratings:** fill in the questionnaire. PicoAmp plays internet radio and
   shows news from the web, so answer "yes" to unrestricted internet access.
4. **Packages:** upload the `.msix` file.
5. **Store listings:** use the text below and screenshots (the ones in `docs/`
   are 768 x 576; the Store wants at least 1366 x 768, so take them from the
   window, for example full screen with F11).
6. **Submission options:** the package uses `runFullTrust` (an ordinary desktop
   program). Explain it in the notes for certification: "A Win32 desktop app
   packaged as MSIX; runFullTrust is needed for any desktop app".

Certification usually takes from a few hours to a few days.

## Store listing text (to copy)

**Short description**

> The MSX2 music player PICOAMP on Windows: music files, internet radio,
> spectrum analyser, clock, weather and news, pixel for pixel as on the MSX.

**Description**

> PicoAmp brings PICOAMP, the music and internet radio player of the PicoVerse
> 2350 cartridge for MSX2 computers, to Windows. Every page is drawn as on the
> MSX, with the same colours, font and keys, sharp at any window size.
>
> • Plays MP3, WAV and FLAC files from your folders, with title and artist
> • Internet radio: thousands of stations from the free Radio Browser
>   directory (by country, genre, decade, or search), MP3 and AAC, with the
>   song titles; your own station lists as .m3u files; favourites
> • A 24-band spectrum analyser, VU meters, an oscilloscope, a 5-band
>   equalizer and full-screen pictures (bars, mirror, plasma)
> • A clock (digital or analog) in colours that move with the music, with a
>   sleep timer and an alarm
> • The weather now and for the next days (Open-Meteo)
> • A news reader for RSS and Atom feeds, with the full articles, in the MSX's
>   80-column text mode; your feeds as an OPML file
> • Media keys, drag and drop, full screen, scan lines
>
> Free and open (CC BY-NC-SA 4.0), by JZZY & Claude, after PICOAMP for the MSX
> PicoVerse 2350. Source: https://github.com/Jzzy-dev/win-picoamp

**Features** (one per line)

> Music files: MP3, WAV, FLAC
> Internet radio with a directory of thousands of stations
> Spectrum analyser, oscilloscope and equalizer
> Clock with sleep timer and alarm
> Weather and news reader
> The look of the MSX2, sharp at any size

**Privacy policy** (the Store asks for a URL: this file on GitHub will do)

> PicoAmp collects no personal data and has no account, tracking or
> advertising. It connects to the internet only for what you use:
> * the radio stations you play
> * the Radio Browser directory, which is told when a station from it starts playing
> * ip-api.com (the place of your internet connection, for the clock and the weather) and Open-Meteo (the weather)
> * the news feeds and articles you open
>
> Those services see your IP address, as any website does. The settings are
> stored on your PC only.
