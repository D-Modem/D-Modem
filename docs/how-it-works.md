# How it works

Traditional "controller-based" modems had a microcontroller and a DSP on the device. These two components did all of the work of the modem. Then "Winmodems" came into use. A Winmodem had a DSP that the host PC loads with its program. The controller and other functions moved into software on the host PC. After that, "pure software" modems (softmodems) also moved the functions of the DSP to the host. The hardware of a softmodem only connects to the telephone network. The software does all of the processing.

D-Modem replaces the hardware of a softmodem with a SIP stack. The audio does not go between the software DSP and an analogue telephone line. Instead, the audio goes through the RTP (or SRTP) media streams of a SIP call. D-Modem can also operate the hardware of some softmodems on a real telephone line ([Softmodem hardware](hardware.md)).
