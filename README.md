# SendStereoMixToPulseAudioServer

A Windows utility that captures system audio (Stereo Mix / loopback) via WASAPI and streams it in real-time
over the network to a PulseAudio server.

## Configure Pulseaudio on server in system wide mode

Execute to list all sinks in system
```
pactl list sinks short
0       alsa_output.platform-1c22c00.codec.stereo-fallback      module-alsa-card.c      s16le 2ch 44100Hz       IDLE
1       alsa_output.platform-sound.stereo-fallback      module-alsa-card.c      s16le 2ch 44100Hz       RUNNING
```

Create Pulseaudio system wide add-in config
```
cat >/etc/pulse/system.pa.d/local.pa <<EOF
load-module module-zeroconf-publish
load-module module-native-protocol-tcp auth-anonymous=1
unload-module module-suspend-on-idle
set-default-sink alsa_output.platform-sound.stereo-fallback
EOF
```

Disable Pulseaudio in user mode
```
systemctl --user disable pulseaudio.service
```

Create systemd service for PulseAudio server in system wide mode
```
cat >/etc/systemd/system/pulseaudio.service <<EOF
[Unit]
Description=PulseAudio system server

[Service]
Type=notify
ExecStartPre=/bin/sleep 4
ExecStart=pulseaudio --daemonize=no --system --realtime --log-target=journal --disallow-exit --no-cpu-limit

[Install]
WantedBy=multi-user.target
EOF
```

Enable Pulseaudio in system wide mode
```
systemctl enable pulseaudio.service
```

## At Windows
Execute
```
SendStereoMixToPulseAudioServer.exe <server-name-or-ip>
```
Streaming should start immediately.
Use tray icon to stop streaming and exit

To add utility to Windows autostart. Modify AddToAutostart.bat as required and execute it.


## Compiling
Download and install MSYS2 build system - msys2-x86_64-20260322.exe
Install packages
pacman -S mingw-w64-ucrt-x86_64-pulseaudio
pacman -S mingw-w64-ucrt-x86_64-gcc
Compile
windres.exe SendStereoMixToPulseAudioServer.rc SendStereoMixToPulseAudioServer.rc.o
gcc -mwindows -municode -O3 SendStereoMixToPulseAudioServer.cpp SendStereoMixToPulseAudioServer.rc.o -l:libavrt.a -l:libmfplat.a -l:libole32.a -llibpulse-simple -o SendStereoMixToPulseAudioServer.exe
strip.exe SendStereoMixToPulseAudioServer.exe 