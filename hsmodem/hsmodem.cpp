/*
* High Speed modem to transfer data in a 2,7kHz SSB channel
* =========================================================
* Author: DJ0ABR
* made for: AMSAT-DL
*
*   (c) DJ0ABR
*   www.dj0abr.de
*
*   This program is free software; you can redistribute it and/or modify
*   it under the terms of the GNU General Public License as published by
*   the Free Software Foundation; either version 2 of the License, or
*   (at your option) any later version.
*
*   This program is distributed in the hope that it will be useful,
*   but WITHOUT ANY WARRANTY; without even the implied warranty of
*   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
*   GNU General Public License for more details.
*
*   You should have received a copy of the GNU General Public License
*   along with this program; if not, write to the Free Software
*   Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
*
*/

/*
* this is a console program
* it can be compiled under Linux: make
* and under Windows: Visual-Studio
* 
* 3rd party libraries:
* 1) BASS Audio from https://www.un4seen.com/
	copy bass.h and bass.lib into source directory
	Windows: copy bass.dll into executable directory
	Linux: copy libbass.so into shared-lib folder, usually /usr/local/lib
	! NOTE: for PC-Linux and ARM-Linux you need different libraries !

2) liquid-DSP
	Linux Install Script:
		this installs it from source

		sudo apt install git autoconf libsndfile-dev libasound-dev
		git clone git://github.com/jgaeddert/liquid-dsp.git
		cd liquid-dsp
		./bootstrap.sh
		./configure
		make -j 8
		sudo make install
		sudo ldconfig

		a working copy of the source code is in ../3rdParty/liquid-dsp
		to use this source simply remove the "git clone" line from above script
		it installs libliquid.so into /usr/local/lib (Ubuntu) and
		liquid.h into /usr/local/include/liquid/

	Windows:
		ready libraries are in ../3rdParty/liquid-dsp-windows
		copy liquid.h and liquid.lib into source directory
		copy liquid.dll into executable directory
*/


#include "hsmodem.h"

void toGR_sendData(uint8_t* data, int type, int status);
void bc_rxdata(uint8_t* pdata, int len, struct sockaddr_in* rxsock);
void appdata_rxdata(uint8_t* pdata, int len, struct sockaddr_in* rxsock);
void startModem();

// threads will exit if set to 0
int keeprunning = 1;

// UDP I/O
int BC_sock_AppToModem = -1;
int DATA_sock_AppToModem = -1;
int DATA_sock_from_GR = -1;
int DATA_sock_FFT_from_GR = -1;
int DATA_sock_I_Q_from_GR = -1;

int UdpBCport_AppToModem = 40131;
int UdpDataPort_AppToModem = 40132;
int UdpDataPort_ModemToApp = 40133;

int UdpDataPort_toGR = 40134;
int UdpDataPort_fromGR = 40135;
int UdpDataPort_fromGR_FFT = 40136;
int UdpDataPort_fromGR_I_Q = 40137;

// op mode depending values
// default mode if not set by the app
int speedmode = 7;
int bitsPerSymbol = 2;      // QPSK=2, 8PSK=3
int constellationSize = 4;  // QPSK=4, 8PSK=8

char localIP[] = { "127.0.0.1" };
char ownfilename[] = { "hsmodem" };
char appIP[20] = { 0 };
int fixappIP = 0;
int restart_modems = 0;

int caprate = 44100;
int txinterpolfactor = 20;
int rxPreInterpolfactor = 5;

int captureDeviceNo = -1;
int playbackDeviceNo = -1;

int main(int argc, char* argv[])
{
    int opt = 0;
    char* modemip = NULL;

#ifdef _LINUX_
    while ((opt = getopt(argc, argv, "m:")) != -1)
    {
        switch (opt)
        {
        case 'm':
            // specify IP of application: hsmodem -m 192.168.0.1
            modemip = optarg;
            memset(appIP, 0, 20);
            int len = strlen(modemip);
            if (len < 16)
            {
                memcpy(appIP, modemip, len);
                fixappIP = 1;
                printf("Application IP set to: %s\n", modemip);
            }
            else
            {
                printf("invalid Application IP: %s\n", modemip);
                exit(0);
            }
            break;
        }
    }

    if (isRunning(ownfilename) == 1)
        exit(0);

    install_signal_handler();
#endif

#ifdef _WIN32_
    if (argc != 1 && argc != 3)
    {
        printf("invalid argument\n");
        exit(0);
    }
    if (argc == 3)
    {
        memset(appIP, 0, 20);
        int len = strlen(argv[2]);
        if (len < 16)
        {
            memcpy(appIP, argv[2], len);
            fixappIP = 1;
            printf("Application IP set to: %s\n", argv[2]);
        }
        else
        {
            printf("invalid Application IP: %s\n", modemip);
            exit(0);
        }
    }
#endif
    init_packer();
    
    initFEC();
    init_fft();
    int ar = init_audio(playbackDeviceNo, captureDeviceNo);
    if (ar == -1)
    {
        keeprunning = 0;
        exit(0);
    }

    // start udp RX to listen for broadcast search message from Application
    UdpRxInit(&BC_sock_AppToModem, UdpBCport_AppToModem, &bc_rxdata, &keeprunning);

    // start udp RX for data from application
    UdpRxInit(&DATA_sock_AppToModem, UdpDataPort_AppToModem, &appdata_rxdata, &keeprunning);

    // start udp RX to listen for data from GR Receiver
    UdpRxInit(&DATA_sock_from_GR, UdpDataPort_fromGR, &GRdata_rxdata, &keeprunning);

    printf("QO100modem initialised and running\n");

    while (keeprunning)
    {
        if (restart_modems == 1)
        {
            startModem();
            restart_modems = 0;
        }

        //doArraySend();

        if (demodulator() == 0)
            sleep_ms(100);
    }
    printf("stopped: %d\n", keeprunning);

#ifdef _LINUX_
    close(BC_sock_AppToModem);
#endif
#ifdef _WIN32_
    closesocket(BC_sock_AppToModem);
#endif


    return 0;
}


typedef struct {
    int audio;
    int tx;
    int rx;
    int bpsym;
} SPEEDRATE;

SPEEDRATE sr[10] = {
    // QPSK modes
    {48000, 32, 8, 2}, // AudioRate, TX-Resampler, RX-Resampler/4, bit/symbol
    {44100, 28, 7, 2}, // see samprate.ods
    {44100, 24, 6, 2},
    {48000, 24, 6, 2},
    {44100, 20, 5, 2},
    {48000, 20, 5, 2},

    // 8PSK modes
    {44100, 24, 6, 3},
    {48000, 24, 6, 3},
    {44100, 20, 5, 3},
    {48000, 20, 5, 3}
};

void startModem()
{
    bitsPerSymbol = sr[speedmode].bpsym;
    constellationSize = (1 << bitsPerSymbol); // QPSK=4, 8PSK=8

    caprate = sr[speedmode].audio;
    txinterpolfactor = sr[speedmode].tx;
    rxPreInterpolfactor = sr[speedmode].rx;

    // int TX audio and modulator
    close_dsp();
    init_audio(playbackDeviceNo, captureDeviceNo);
    init_dsp();
}

void setAudioDevices(int pb, int cap)
{
    //printf("%d %d\n", pb, cap);

    if (pb != playbackDeviceNo || cap != captureDeviceNo)
    {
        restart_modems = 1;
        playbackDeviceNo = pb;
        captureDeviceNo = cap;
    }
}

// called from UDP RX thread for Broadcast-search from App
void bc_rxdata(uint8_t* pdata, int len, struct sockaddr_in* rxsock)
{
    if (len > 0 && pdata[0] == 0x3c)
    {
        setAudioDevices(pdata[1], pdata[2]);

        char rxip[20];
        strcpy(rxip, inet_ntoa(rxsock->sin_addr));

        if (fixappIP == 0)
        {
            if (strcmp(appIP, rxip))
            {
                printf("new app IP: %s, restarting modems\n", rxip);
                restart_modems = 1;
            }
            strcpy(appIP, rxip);
            //printf("app (%s) is searching modem. Sending modem IP to the app\n",appIP);
            // App searches for the modem IP, mirror the received messages
            // so the app gets an UDP message with this local IP
            int alen;
            uint8_t* txdata = getAudioDevicelist(&alen);
            sendUDP(appIP, UdpDataPort_ModemToApp, txdata, alen);
        }
        else
        {
            // appIP is fixed, answer only to this IP
            if (!strcmp(appIP, rxip))
            {
                //printf("app (%s) is searching modem. Sending modem IP to the app\n",appIP);
                restart_modems = 1;
                // App searches for the modem IP, mirror the received messages
                // so the app gets an UDP message with this local IP
                int alen;
                uint8_t* txdata = getAudioDevicelist(&alen);
                sendUDP(appIP, UdpDataPort_ModemToApp, txdata, alen);
            }
        }
    }
}

// called by UDP RX thread for data from App
void appdata_rxdata(uint8_t* pdata, int len, struct sockaddr_in* rxsock)
{
    uint8_t type = pdata[0];
    uint8_t minfo = pdata[1];

    if (len != (PAYLOADLEN + 2))
    {
        printf("data from app: wrong length:%d (should be %d)\n", len - 2, PAYLOADLEN);
        return;
    }

    // type values: see oscardata config.cs: frame types
    if (type == 16)
    {
        // Byte 1 contains the resampler ratio for TX and RX modem
        speedmode = pdata[1];
        printf("set speedmode to %d\n", speedmode);
        restart_modems = 1;
        return;
    }

    if (type == 17)
    {
        // auto send file
        // TODO

        // for testing only:
        // simulate sending a text file with 1kB length
        /*int testlen = 100000;
        uint8_t arr[100000];
        char c = 'A';
        for (int i = 0; i < testlen; i++)
        {
            arr[i] = c;
            if (++c > 'Z') c = 'A';
        }
        arraySend(arr, testlen, 3, (char*)"testfile.txt");*/
        return;
    }
    if (type == 18)
    {
        // auto send folder
        // TODO
    }

    if (type == 19)
    {
        // shut down this modem PC
        int r = system("sudo shutdown now");
        exit(r);
    }

    if (type == 20)
    {
        // reset liquid RX modem
        resetModem();
    }

    //if (getSending() == 1) return;   // already sending (Array sending)

    if (minfo == 0)
    {
        // this is the first frame of a larger file
        // send it multiple times, like a preamble, to give the
        // receiver some time for synchronisation
        // duration: 3 seconds
        // caprate: samples/s. This are symbols: caprate/txinterpolfactor
        // and bits: symbols * bitsPerSymbol
        // and bytes/second: bits/8 = (caprate/txinterpolfactor) * bitsPerSymbol / 8
        // one frame has 258 bytes, so we need for 5s: 5* ((caprate/txinterpolfactor) * bitsPerSymbol / 8) /258 + 1 frames
        int numframespreamble = 3 * ((caprate / txinterpolfactor) * bitsPerSymbol / 8) / 258 + 1;
        for (int i = 0; i < numframespreamble; i++)
            toGR_sendData(pdata + 2, type, minfo);
    }
    else if ((len - 2) < PAYLOADLEN)
    {
        // if not enough data for a full payload add Zeros
        uint8_t payload[PAYLOADLEN];
        memset(payload, 0, PAYLOADLEN);
        memcpy(payload, pdata + 2, len - 2);
        toGR_sendData(payload, type, minfo);
    }
    else
    {
        toGR_sendData(pdata + 2, type, minfo);
    }
}

void toGR_sendData(uint8_t* data, int type, int status)
{
    int len = 0;
    uint8_t* txdata = Pack(data, type, status, &len);

    //showbytestring((char *)"BERtx: ", txdata, len);

    if (txdata != NULL)
        sendToModulator(txdata, len);
}

// called by UDP RX thread or liquid demodulator for received data
void GRdata_rxdata(uint8_t* pdata, int len, struct sockaddr_in* rxsock)
{
    static int fnd = 0;

    // raw symbols
    uint8_t* pl = unpack_data(pdata, len);
    if (pl != NULL)
    {
        // complete frame received
        // send payload to app
        uint8_t txpl[PAYLOADLEN + 10 + 1];
        memcpy(txpl + 1, pl, PAYLOADLEN + 10);
        txpl[0] = 1;    // type 1: payload data follows
        sendUDP(appIP, UdpDataPort_ModemToApp, txpl, PAYLOADLEN + 10 + 1);
        fnd = 0;
    }
    else
    {
        // no frame found
        // if longer ws seconds nothing found, reset liquid RX modem
        // comes here with symbol rate, i.e. 4000 S/s
        int ws = 2;
        int wt = sr[speedmode].audio / sr[speedmode].tx;
        if (++fnd >= (wt * ws))
        {
            fnd = 0;
            //printf("no signal detected %d, reset RX modem\n", wt);
            resetModem();
        }
    }
}