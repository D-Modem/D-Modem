# How it works

Traditional “controller-based” modems usually had a microcontroller and a DSP on the device. These two components did all of the modem communication.  Then, “Winmodems” came into use. Winmodems had DSPs that you can program in the field. They moved the controller and other functions into software on the host PC.  After that, “pure software” modems moved the DSP functions to the host also.  The physical hardware of these softmodems only connected to the telephone network. The software did all of the processing.

D-Modem replaces the physical hardware of a softmodem with a SIP stack.  The audio does not go between the software DSP and an analog telephone line. Instead, the audio goes through the RTP (or SRTP) media streams of a SIP VoIP call.
