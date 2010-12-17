/** ModularEEG acqusition tool to stream data to a FieldTrip buffer,
	and write data to one or multiple GDF files (if you ever reach the size limit...).
	(C) 2010 S. Klanke
*/
#include <serial.h>

#include <OnlineDataManager.h>
#include <ConsoleInput.h>
#include <StringServer.h>

#define NUM_HW_CHAN 6
#define FSAMPLE     256
#define PACKET_LEN  17

int main(int argc, char *argv[]) {
	int sampleCounter = 0;
	char hostname[256];
	int port;
	short switchState = 0;
	StringServer ctrlServ;
	ConsoleInput ConIn;
	SerialPort SP;
	unsigned char serialBuffer[1024];
	/*
	short sampleData[NUM_HW_CHAN * FSAMPLE]; // holds up to 1 seconds of data
	short switchData[FSAMPLE]; // again, 1 second of data (switches)
	*/
	int leftOverBytes = 0;
	int keepRunning = 1;
	
	if (argc<3) {
		printf("Usage: modeeg2ft <device> <config-file> [hostname=localhost [port=1972]]\n");
		return 0;
	}
	
	OnlineDataManager<short,float> ODM(1, NUM_HW_CHAN, FSAMPLE);
	ctrlServ.startListening(8000);
	
	int err = ODM.configureFromFile(argv[2]);
	if (err == -1) {
		fprintf(stderr, "Could not read configuration file %s\n", argv[2]);
		return 1;
	}
	if (err > 0) {
		fprintf(stderr, "Encountered %i errors in configuration file - aborting\n", err);
		return 1;
	}

	if (argc>3) {
		strncpy(hostname, argv[3], sizeof(hostname));
	} else {
		strcpy(hostname, "localhost");
	}
	
	if (argc>4) {
		port = atoi(argv[4]);
	} else {
		port = 1972;
	}
	
	if (!serialOpenByName(&SP, argv[1])) {
		fprintf(stderr, "Could not open serial port %s\n", argv[1]);
		return 1;
	}

	// last parameter is timeout in 1/10 of a second
	if (!serialSetParameters(&SP, 57600, 8, 0, 1, 0)) {
		fprintf(stderr, "Could not modify serial port parameters\n");
		return 1;
	}

	if (!strcmp(hostname, "-")) {
		if (!ODM.useOwnServer(port)) {
			fprintf(stderr, "Could not spawn buffer server on port %d.\n",port);
			return 0;
		}
	} else {
		if (!ODM.connectToServer(hostname, port)) {
			fprintf(stderr, "Could not connect to buffer server at %s:%d.\n",hostname, port);
			return 0;
		}
	}	
	
	// printf("Looking for sync bytes 0xA5 0x5A (%c%c)\n", 0xA5, 0x5A);
	
	// read bytes until we get 0xA5,0x5A,...
	for (int iter = 0;iter < 200; iter++) {
		unsigned char byte;
		int nr;
		
		nr = serialRead(&SP, 1, &byte);
		if (nr<0) {
			fprintf(stderr, "Error when reading from serial port - exiting\n");
			return 1;
		} 
		if (nr==0) {
			printf(".");
			usleep(10000);	// sleep for 10 ms if no byte received
			continue;
		}
		
		//printf("%02X %c\n", byte, byte);
		
		if (byte == 0xA5) {
			serialBuffer[0] = byte;
			leftOverBytes = 1;
		} else if (leftOverBytes == 1 && byte == 0x5A) {
			serialBuffer[1] = byte;
			leftOverBytes = 2;
			break; // success !
		} else {
			leftOverBytes = 0;
		}
	}
	if (leftOverBytes != 2) {
		fprintf(stderr, "Could not read synchronisation bytes from ModularEEG\n");
		goto cleanup;
	}
	
	printf("Got synchronization bytes - starting acquisition\n");
	printf("\nPress <Esc> to quit\n\n");
	
	while (keepRunning) {
		int numRead, numTotal, numSamples, numPending, maxReadNow;
		
		if (ConIn.checkKey()) {
			int c = ConIn.getKey();
			if (c==27) break; // quit
		}
		
		ctrlServ.checkRequests(ODM);
		
		numPending = serialInputPending(&SP);
		if (numPending < 0) {
			fprintf(stderr, "Error when reading from serial port - exiting\n");
			break;
		}
		if (numPending == 0) {
			usleep(10000);
			continue;
		}
		
		maxReadNow = sizeof(serialBuffer) - leftOverBytes;
		if (numPending > maxReadNow) {
			numPending = maxReadNow;
		}
		
		numRead = serialRead(&SP, numPending, serialBuffer + leftOverBytes);
		if (numRead != numPending) {
		    fprintf(stderr, "Error when reading from serial port - exiting\n");
			break;
		}	
		
		numTotal   = leftOverBytes + numRead;
		numSamples = numTotal / PACKET_LEN;
		
		if (numSamples > FSAMPLE) {
			fprintf(stderr, "Received too much data from serial port - exiting.\n");
			break;
		}
		
		if (numSamples == 0) {
			leftOverBytes += numRead;
			continue;
		}
		
		short *block = ODM.provideBlock(numSamples);
		
		// first decode into switch + data 
		for (int j=0;j<numSamples;j++) {
			int soff = j*PACKET_LEN;
			int doff = j*(1+NUM_HW_CHAN);
			
			if (serialBuffer[soff] != 0xA5 || serialBuffer[soff+1] != 0x5A) {
				fprintf(stderr, "ModularEEG out of sync in sample %i - exiting.\n", sampleCounter + j);
				keepRunning = 0;
				break;
			}
						
			short switchVal = serialBuffer[soff+16];
			if (switchVal != switchState) {
				switchState = switchVal;
				if (switchState!=0) {
					ODM.getEventList().add(j, "Switch", switchState);
				}
			}
			block[doff] = switchVal;
			
			for (int i=0;i<NUM_HW_CHAN;i++) {
				short sampleData = serialBuffer[soff+4+2*i]*256 + serialBuffer[soff+5+2*i];
				block[doff + 1 + i] = sampleData;
			}
		}
		
		if (!ODM.handleBlock()) {
			fprintf(stderr, "Error in handling this data block - stopping\n");
			break;
		}

		sampleCounter += numSamples;
		leftOverBytes = numTotal - numSamples * PACKET_LEN;
		
		// copy left-over bytes to the beginning of the serialBuffer
		if (leftOverBytes > 0) {
			memcpy(serialBuffer, serialBuffer + numSamples*PACKET_LEN, leftOverBytes);
		}
	}

cleanup:	
	serialClose(&SP);
	return 0;
}
