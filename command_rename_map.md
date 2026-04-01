# Command Rename Map: Space-separated → Single-word

## System (System_Utils.cpp - commands[])
- `pending list` → `pendinglist`

## Battery (System_Utils.cpp - batteryCommands[])
- `battery status` → `batterystatus`
- `battery calibrate` → `batterycalibrate`

## Users (System_User.cpp - userSystemCommands[])
- `user approve` → `userapprove`
- `user deny` → `userdeny`
- `user promote` → `userpromote`
- `user demote` → `userdemote`
- `user delete` → `userdelete`
- `user changepassword` → `userchangepassword`
- `user resetpassword` → `userresetpassword`
- `user add` → `useradd`
- `user list` → `userlist`
- `user request` → `userrequest`
- `user sync` → `usersync`
- `session list` → `sessionlist`
- `session revoke` → `sessionrevoke`

## ESP-NOW (System_ESPNow.cpp - espNowCommands[])
- `espnow stats` → `espnowstats`
- `espnow routerstats` → `espnowrouterstats`
- `espnow broadcaststats` → `espnowbroadcaststats`
- `espnow resetstats` → `espnowresetstats`
- `espnow pair` → `espnowpair`
- `espnow unpair` → `espnowunpair`
- `espnow list` → `espnowlist`
- `espnow meshstatus` → `espnowmeshstatus`
- `espnow meshmetrics` → `espnowmeshmetrics`
- `espnow mode` → `espnowmode`
- `espnow meshttl` → `espnowmeshttl`
- `espnow setname` → `espnowsetname`
- `espnow hbmode` → `espnowhbmode`
- `espnow meshrole` → `espnowmeshrole`
- `espnow meshmaster` → `espnowmeshmaster`
- `espnow meshbackup` → `espnowmeshbackup`
- `espnow backupenable` → `espnowbackupenable`
- `espnow meshtopo` → `espnowmeshtopo`
- `espnow toporesults` → `espnowtoporesults`
- `espnow timesync` → `espnowtimesync`
- `espnow timestatus` → `espnowtimestatus`
- `espnow meshsave` → `espnowmeshsave`
- `espnow room` → `espnowroom`
- `espnow zone` → `espnowzone`
- `espnow tags` → `espnowtags`
- `espnow friendlyname` → `espnowfriendlyname`
- `espnow stationary` → `espnowstationary`
- `espnow deviceinfo` → `espnowdeviceinfo`
- `espnow devices` → `espnowdevices`
- `espnow rooms` → `espnowrooms`
- `espnow find` → `espnowfind`
- `espnow roomcmd` → `espnowroomcmd`
- `espnow tagcmd` → `espnowtagcmd`
- `espnow send` → `espnowsend`
- `espnow broadcast` → `espnowbroadcast`
- `espnow sendfile` → `espnowsendfile`
- `espnow browse` → `espnowbrowse`
- `espnow fetch` → `espnowfetch`
- `espnow remote` → `espnowremote`
- `espnow worker` → `espnowworker`
- `espnow sensorstream` → `espnowsensorstream`
- `espnow sensorstatus` → `espnowsensorstatus`
- `espnow sensorbroadcast` → `espnowsensorbroadcast`
- `espnow usersync` → `espnowusersync`
- `espnow requestmeta` → `espnowrequestmeta`
- `espnow setpassphrase` → `espnowsetpassphrase`
- `espnow encstatus` → `espnowencstatus`
- `espnow pairsecure` → `espnowpairsecure`
- `espnow buffers` → `espnowbuffers`
- `test streams` → `teststreams`
- `test concurrent` → `testconcurrent`
- `test cleanup` → `testcleanup`
- `test filelock` → `testfilelock`

## Bond (System_ESPNow.cpp - espNowCommands[], inside #if ENABLE_BONDED_MODE)
- `bond connect` → `bondconnect`
- `bond disconnect` → `bonddisconnect`
- `bond status` → `bondstatus`
- `bond role` → `bondrole`
- `bond showcap` → `bondshowcap`
- `bond requestcap` → `bondrequestcap`
- `bond showmanifest` → `bondshowmanifest`
- `bond requestmanifest` → `bondrequestmanifest`
- `bond showremotemanifest` → `bondshowremotemanifest`
- `bond stream` → `bondstream`
- `bond testsensor` → `bondtestsensor`

## ESP-SR (System_ESPSR.cpp - espsrCommands[])
- `sr enable` → `srenable`
- `sr start` → `srstart`
- `sr stop` → `srstop`
- `sr status` → (already exists as `srstatus`)
- `sr cmds` → `srcmds`
- `sr cmds list` → `srcmdslist`
- `sr cmds add` → `srcmdsadd`
- `sr cmds del` → `srcmdsdel`
- `sr cmds clear` → `srcmdsclear`
- `sr cmds reload` → `srcmdsreload`
- `sr cmds save` → `srcmdssave`
- `sr cmds sync` → `srcmdssync`
- `sr debug` → `srdebug`
- `sr debug level` → `srdebuglevel`
- `sr debug telem` → `srdebugtelem`
- `sr debug stats` → `srdebugstats`
- `sr debug reset` → `srdebugreset`
- `sr confidence` → `srconfidence`
- `sr accept` → `sraccept`
- `sr dyngain` → `srdyngain`
- `sr raw` → `srraw`
- `sr autotune` → `srautotune`
- `sr timeout` → `srtimeout`
- `sr tuning` → `srtuning`
- `sr tuning swgain` → `srtuningswgain`
- `sr tuning gain` → `srtuninggain`
- `sr tuning agc` → `srtuningagc`
- `sr tuning vad` → `srtuningvad`
- `sr tuning filters` → `srtuningfilters`
- `sr snip` → `srsnip`
- `sr snip on` → `srsnipon`
- `sr snip off` → `srsnipoff`
- `sr snip start` → `srsnipstart`
- `sr snip stop` → `srsnipstop`
- `sr snip status` → `srsnipstatus`
- `sr snip config` → `srsnipconfig`
- `voice arm` → `voicearm`
- `voice disarm` → `voicedisarm`
- `voice status` → `voicestatus`
- `voice cancel` → `voicecancel`
- `voice help` → `voicehelp`

## Edge Impulse (System_EdgeImpulse.cpp - edgeImpulseCommands[])
- `ei enable` → `eienable`
- `ei detect` → `eidetect`
- `ei file` → `eifile`
- `ei continuous` → `eicontinuous`
- `ei confidence` → `eiconfidence`
- `ei status` → `eistatus`
- `ei model` → `eimodel`
- `ei model list` → `eimodellist`
- `ei model load` → `eimodelload`
- `ei model info` → `eimodelinfo`
- `ei model unload` → `eimodelunload`
- `ei track` → `eitrack`
- `ei track status` → `eitrackstatus`
- `ei track enable` → `eitrackenable`
- `ei track clear` → `eitrackclear`

## FM Radio (i2csensor-fmradio.cpp)
- `fmradio tune` → `fmradiotune`
- `fmradio seek` → `fmradioseek`
- `fmradio volume` → `fmradiovolume`
- `fmradio mute` → `fmradiomute`
- `fmradio unmute` → `fmradiounmute`

## G2 Glasses (Optional_EvenG2.cpp - g2Commands[])
- `g2 clear` → `g2clear`
- `g2 deinit` → `g2deinit`
- `g2 init` → `g2init`
- `g2 nav` → `g2nav`
- `g2 scan` → `g2scan`
- `g2 show` → `g2show`
- `g2 verbose` → `g2verbose`

## Set Pattern (OLED_Mode_SetPattern.cpp)
- `set gamepad password` → `setgamepadpassword`

## STATUS
- [x] System_Utils.cpp (commands[], batteryCommands[])
- [x] System_User.cpp (userSystemCommands[])
- [x] System_ESPNow.cpp (espNowCommands[])
- [x] System_ESPSR.cpp (espsrCommands[])
- [x] System_EdgeImpulse.cpp (edgeImpulseCommands[])
- [x] i2csensor-rda5807.cpp (fmRadioCommands[])
- [x] Optional_EvenG2.cpp (g2Commands[])
- [x] OLED_Mode_SetPattern.cpp (setPatternCommands[])
- [x] System_LLM.cpp (llmCommands[])
- [x] Update all Usage strings
- [x] Update all JS/web references (OLED_ESPNow, OLED_Utils, OLED_Mode_Remote, WebPage_Bond, WebServer_Server, HardwareOne)
- [x] Update automation references
- [ ] Build test
