
#include "pch.h"
#include "rs232.h"
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <sstream>
#include <string>
#include <windows.h>
#include <mmdeviceapi.h>
#include <tchar.h>
#include <endpointvolume.h>
#include <audiopolicy.h>
#include <Psapi.h>
#include <WinUser.h>
#include <Functiondiscoverykeys_devpkey.h>
#include <conio.h>

#include "../../common/serialprotocol.h"

const uint8_t corsair[] =
    {
    0x00, 0x00,
    0x00, 0x20,
    0x04, 0x30,
    0x02, 0x18,
    0x22, 0x40,
    0x10, 0x60,
    0x06, 0x70,
    0x03, 0x78,
    0x33, 0x7c,
    0x33, 0x7e,
    0x37, 0xfe,
    0x3f, 0xce,
    0x3c, 0x00,
    0x60, 0x00,
    0x00, 0x00,
    0x00, 0x00
    };

//Constants
#define MAX_GROUPS 128
#define MAX_STREAMS 128

//Types
typedef struct
{
	int streamIndex;
	IAudioSessionControl	*pSessionControl;
	IAudioSessionControl2	*pSessionControl2;
	ISimpleAudioVolume		*pVolumeControl;
	GUID					guid;

	PWSTR					displayName;
	WCHAR					prettyName[MAX_PATH];

    float                   prevVolume;
    int                     update;

}groupData_t;

//Globals
groupData_t groups[MAX_GROUPS];

int cport_nr = 5;        /* /dev/ttyS0 (COM1 on windows) */
int bdrate = 19200;

#define serialSendBuffer(_dPtr, _dCount)	RS232_SendBuf(cport_nr, _dPtr, _dCount)

serialProtocol_t * allocProtocolBuf(msgtype_t, size_t);
void freeProtocolBuf(serialProtocol_t **);


wchar_t deviceName[MAX_PATH];


//Functions 
int getGroups(IAudioSessionEnumerator *, groupData_t *);
void getLabels(IAudioSessionEnumerator *, groupData_t *, int);
void sendChannelInfo(int, float);
void sendMasterInfo(float);

HWND g_HWND = NULL;
BOOL CALLBACK EnumWindowsProcMy(HWND hwnd, LPARAM lParam)
{
	DWORD lpdwProcessId;
    g_HWND = NULL;
	GetWindowThreadProcessId(hwnd, &lpdwProcessId);
	if (lpdwProcessId == lParam)
	{
        //Find the top level
        while (1)
            {
            if (GetParent(hwnd))
                {
                hwnd = GetParent(hwnd);
                }
            else
                {
                break;
                }
            }
		g_HWND = hwnd;
		return FALSE;
	}
	return TRUE;
}

void Usage()
{
	printf("Usage: \n");
	printf(" SetVolume [Reports the current volume]\n");
	printf(" SetVolume -d <new volume in decibels> [Sets the current default render device volume to the new volume]\n");
	printf(" SetVolume -f <new volume as an amplitude scalar> [Sets the current default render device volume to the new volume]\n");

}

int _tmain(int argc, _TCHAR* argv[])
{
	HRESULT hr;
	bool decibels = false;
	bool scalar = false;
	double newVolume;
	int groupCount = 0;


	if (argc != 3 && argc != 1)
	{
		Usage();
		return -1;
	}
	if (argc == 3)
	{
		if (argv[1][0] == '-')
		{
			if (argv[1][1] == 'f')
			{
				scalar = true;
			}
			else if (argv[1][1] == 'd')
			{
				decibels = true;
			}
		}
		else
		{
			Usage();
			return -1;
		}

		newVolume = _tstof(argv[2]);
	}


	int i = 0;
	char mode[] = { '8','N','1',0 };
	char str[2][512];

	if (RS232_OpenComport(cport_nr, bdrate, mode))
	{
		printf("Can not open serial port\n");
	}
	else
	{
		printf("Serial port %d opened\n", cport_nr + 1);		
        RS232_flushRXTX(cport_nr);
        Sleep(2000); //Wait for the arduino to reset
	}

	// -------------------------
	CoInitialize(NULL);
	IMMDeviceEnumerator *deviceEnumerator = NULL;
	hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), NULL, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), (LPVOID *)&deviceEnumerator);
	IMMDevice *defaultDevice = NULL;

	hr = deviceEnumerator->GetDefaultAudioEndpoint(eRender, eConsole, &defaultDevice);
	deviceEnumerator->Release();
	deviceEnumerator = NULL;

	IAudioEndpointVolume *endpointVolume = NULL;
	hr = defaultDevice->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, NULL, (LPVOID *)&endpointVolume);
    
    LPWSTR devID;
    IPropertyStore *pProps = NULL;
    hr = defaultDevice->GetId(&devID);

    hr = defaultDevice->OpenPropertyStore(
        STGM_READ, &pProps);

    PROPVARIANT varName;
    // Initialize container for property value.
    PropVariantInit(&varName);

    // Get the endpoint's friendly-name property.
    hr = pProps->GetValue(
        PKEY_Device_FriendlyName, &varName);
    wcscpy_s(deviceName, varName.pwszVal);


	// ---------------------------
	// Get the session managers for the endpoint device.	
	IAudioSessionManager2 *pManager2 = NULL;
	hr = defaultDevice->Activate(__uuidof(IAudioSessionManager2), CLSCTX_INPROC_SERVER, NULL, (void**)&pManager2);

	//Release the device, we have what we need
	defaultDevice->Release();
	defaultDevice = NULL;

    //get the sessionenumerator
    IAudioSessionEnumerator *pEnumerator = NULL;
    hr = pManager2->GetSessionEnumerator(&pEnumerator);

    float lastVolume = -1;

    //Enter main loop
    while (!_kbhit())
        {
        // Master volume
        float currentVolume = 0;

        hr = endpointVolume->GetMasterVolumeLevelScalar(&currentVolume);
        printf("Current volume as a scalar is: %f\n", currentVolume);
        if (currentVolume != lastVolume)
            {
            sendMasterInfo(currentVolume * 100);
            lastVolume = currentVolume;
            }

        //Get data and list info about streams		
        groupCount = getGroups(pEnumerator, groups);

        getLabels(pEnumerator, groups, groupCount);


        if (decibels)
            {
            hr = endpointVolume->SetMasterVolumeLevel((float)newVolume, NULL);
            }
        else if (scalar)
            {
            hr = endpointVolume->SetMasterVolumeLevelScalar((float)newVolume, NULL);
            }

        int channelIx = 0;
        for (int i = 0; i < groupCount; i++)
            {
            sendChannelInfo(i, -1);
            }

        Sleep(200);
        }

	//Clean up
	endpointVolume->Release();
	pEnumerator->Release();
	pManager2->Release();

	RS232_CloseComport(cport_nr);

	CoUninitialize();
	return 0;
}



int getGroups(IAudioSessionEnumerator *pEnumerator, groupData_t *groupData)
{

	typedef struct
	{
		IAudioSessionControl *pSessionControl;
		GUID guid;
	}streamList_t;

	streamList_t streams[MAX_STREAMS];
	groupData_t wgroups[MAX_GROUPS];

	HRESULT hr;
	int currentStreamCount = 0;
	int groupCount0 = 0;
	int groupCount = 0;
	int groupCountG = 0;

	hr = pEnumerator->GetCount(&currentStreamCount);

	printf("Current number of streams %d\n", currentStreamCount);

	//Get guids
	for (int i = 0; i < currentStreamCount; i++)
	{
		//Get session control
		hr = pEnumerator->GetSession(i, &streams[i].pSessionControl);

		//Get guid
		hr = streams[i].pSessionControl->GetGroupingParam(&streams[i].guid);
	}

	//Get null guids, directly to the output
	for (int i = 0; i < currentStreamCount; i++)
	{
		if (streams[i].guid == GUID_NULL)
		{
			//Copy the current stream to a group
			groupData[groupCount0++].guid = streams[i].guid;
			groupData[groupCount0++].pSessionControl = streams[i].pSessionControl;

			groupCount0++;
		}
	}

	//Get the remaining guids
	for (int i = 0; i < currentStreamCount; i++)
	{
		if (streams[i].guid != GUID_NULL)
		{
			//Copy the current stream to a group
			wgroups[groupCount].guid = streams[i].guid;
			wgroups[groupCount].pSessionControl = streams[i].pSessionControl;

			groupCount++;
		}
	}

	groupCountG = groupCount0;
	//Compare guids, throw away duplicates
	for (int i = 0; i < groupCount; i++)
	{
		for (int j = 0; j < groupCount; j++)
		{
			if ((wgroups[j].guid != GUID_NULL) && (j != i) && IsEqualGUID(wgroups[i].guid, wgroups[j].guid))
			{
                //Free the duplicate
                wgroups[j].pSessionControl->Release();
				memset(&wgroups[j], 0, sizeof(groupData_t));
			}
		}
	}

	//Collect the remaining unique guids
	for (int i = 0; i < groupCount; i++)
	{
		if (wgroups[i].guid != GUID_NULL)
		{
			groupData[groupCountG].guid = wgroups[i].guid;
			groupData[groupCountG].pSessionControl = wgroups[i].pSessionControl;

			groupCountG++;
		}
	}

	//Total number of groups, get the rest of the data
	for (int i = 0; i < groupCountG; i++)
	{
		//Sessioncontrol2
		groupData[i].pSessionControl->QueryInterface(__uuidof(IAudioSessionControl2), (void**)&groupData[i].pSessionControl2);
		groupData[i].pSessionControl->QueryInterface(__uuidof(ISimpleAudioVolume), (void**)&groupData[i].pVolumeControl);
		groupData[i].pSessionControl->GetDisplayName(&groupData[i].displayName);


		OLECHAR* guidString;
		StringFromCLSID(groupData[i].guid, &guidString);
		printf("Group no %3d, group: %S, name: \"%S\"\n",
			i,
			guidString,
			groupData[i].displayName);
        CoTaskMemFree(guidString);
	}

	printf("Current number of groups %d\n", groupCountG);

	return groupCountG;
}

void getLabels(IAudioSessionEnumerator *pEnumerator, groupData_t *groupData, int groupCount)
{
	HRESULT hr;
    int label;

	//Find the groups that fo not have a good label already
	for (int i = 0; i < groupCount; i++)
	{
        label = 0;
		if (!wcscmp(groupData[i].displayName, L""))
		{
			printf("Group %d has no label", i);
		}
		else
		{
			printf("Group %d has a label: \"%S\"", i, groupData[i].displayName);
			wcscpy_s(groups[i].prettyName, _countof(groups[i].prettyName), groupData[i].displayName);
            label = 1;

            if (wcsstr(groups[i].prettyName, L"AudioSrv.Dll") != NULL)
                {
                wsprintfW(groups[i].prettyName, L"System Sounds");
                }
		}

		//Get process id

		//Get imagename
		DWORD pid;
		hr = groupData[i].pSessionControl2->GetProcessId(&pid);
		HANDLE Handle = OpenProcess(
			PROCESS_QUERY_INFORMATION | PROCESS_VM_READ,
			FALSE,
			pid
		);
		if (Handle)
		{
			WCHAR Buffer[MAX_PATH];
			if (GetProcessImageFileNameW(Handle, Buffer, _countof(Buffer)))
			{

				//WCHAR label[MAX_PATH];
				//hr = GetWindowText((HWND)Handle, label, _countof(label));
				// At this point, buffer contains the full path to the executable
				//printf("%S, ", Buffer);

				wchar_t *res_p;
				TCHAR fullpath[MAX_PATH];
				res_p = _wfullpath(fullpath, Buffer, _countof(fullpath));
                
				//printf("%S, ", fullpath);

				TCHAR drive[3];
				TCHAR dir[256];
				TCHAR fname[256];
				TCHAR ext[256];
				_tsplitpath_s(
					fullpath,
					drive,
					_countof(drive),
					dir,
					_countof(dir),
					fname,
					_countof(fname),
					ext,
					_countof(ext));
				printf(", executable name: \"%S\"", fname);

				if (wcslen(fname) && !label)
				{
					wcscpy_s(groups[i].prettyName, _countof(groups[i].prettyName), fname);
					memset(fname, 0, sizeof(fname));
				}

				if (!EnumWindows(EnumWindowsProcMy, pid))
				{
					if (GetWindowText(g_HWND, Buffer, _countof(Buffer)))
					{
						printf(", window text: \"%S\"", Buffer);
						if (wcslen(Buffer))
						{
							wcscpy_s(groups[i].prettyName, _countof(groups[i].prettyName), Buffer);
							memset(Buffer, 0, sizeof(Buffer));
						}
					}
				}		
			}
			else
			{
				// You better call GetLastError() here
			}
			CloseHandle(Handle);
		}

        //Free memory
        CoTaskMemFree(groups[i].displayName);
		printf(", prettyName: \"%S\"", groups[i].prettyName);
		printf("\n");
	}
}


void sendChannelInfo(int ch, float masterVolume)
{
	size_t numconv;
	char charName[32 * 2];
	uint8_t vol;
	float fvol;
	serialProtocol_t *msg;

	groups[ch].pVolumeControl->GetMasterVolume(&fvol);
    if (fvol != groups[ch].prevVolume)
        {
        groups[ch].update = 1;
        groups[ch].prevVolume = fvol;
        }
    
    if (groups[ch].update)
        {
        groups[ch].update = 0;

        vol = fvol * 100;
        if (masterVolume >= 0.0)
            {
            vol *= masterVolume;
            }

        msg = allocProtocolBuf(MSGTYPE_SET_CHANNEL_VOL_PREC, sizeof(struct msg_set_channel_vol_prec));
        msg->msg_set_channel_vol_prec.channel = ch;
        msg->msg_set_channel_vol_prec.volVal = vol;
        protocolTxData(msg, sizeof(struct msg_set_channel_vol_prec));
        freeProtocolBuf(&msg);

        wcstombs_s(&numconv, charName, groups[ch].prettyName, sizeof(charName));
        charName[31] = 0;

        int len = sizeof(struct msg_set_channel_label) + strlen(charName) + 1;
        msg = allocProtocolBuf(MSGTYPE_SET_CHANNEL_LABEL, len);
        msg->msg_set_channel_label.channel = ch;
        memcpy_s(msg->msg_set_channel_label.str, strlen(charName) + 1, charName, strlen(charName) + 1);
        msg->msg_set_channel_label.strLen = strlen(charName);

        protocolTxData(msg, len);
        freeProtocolBuf(&msg);
        }
}

void sendMasterInfo(float fvol)
{
    char charName[32 * 2];
    size_t numconv;

	serialProtocol_t *msg = allocProtocolBuf(MSGTYPE_SET_MASTER_VOL_PREC, sizeof(struct msg_set_master_vol_prec));	
	msg->msg_set_master_vol_prec.volVal = fvol;
	protocolTxData(msg, sizeof(struct msg_set_master_vol_prec));
	freeProtocolBuf(&msg);

    int size = sizeof(struct msg_set_master_icon) + sizeof(corsair);
    msg = allocProtocolBuf(MSGTYPE_SET_MASTER_ICON, size);
    memcpy(msg->msg_set_master_icon.icon, corsair, sizeof(corsair));
    protocolTxData(msg, size);
    freeProtocolBuf(&msg);

    wcstombs_s(&numconv, charName, deviceName, sizeof(charName));
    charName[31] = 0;

    int len = sizeof(struct msg_set_master_label) + strlen(charName) + 1;
    msg = allocProtocolBuf(MSGTYPE_SET_MASTER_LABEL, len);
    memcpy_s(msg->msg_set_master_label.str, strlen(charName) + 1, charName, strlen(charName)+1);
    msg->msg_set_master_label.strLen = strlen(charName);

    protocolTxData(msg, len);
    freeProtocolBuf(&msg);

}

serialProtocol_t * allocProtocolBuf(msgtype_t msgType, size_t bytes)
{
	serialProtocol_t *blockPtr = 0;

	blockPtr = (serialProtocol_t*)malloc(bytes);

	blockPtr->msgType = msgType;

	return blockPtr;
}

void freeProtocolBuf(serialProtocol_t **blockBtr)
{
	free(*blockBtr);
    *blockBtr = NULL;
}

#ifdef serialSendBuffer
static void protocolTxData(void *dataPtr, int dataLength)
{
	int numData = 0;
	int i;
	int totalData = 0;
	uint16_t checksum;
	uint8_t *txBufPtr;
	uint8_t *workBufPtr;
		
	//Fill the work buffer
	workBufPtr = msgBuffer;

	//Start the message by adding the length
	*((uint16_t*)workBufPtr) = dataLength;
	numData += 2;
	workBufPtr += 2;	//Jump to the first data byte
	
	//Copy the data
	memcpy_s(workBufPtr, sizeof(msgBuffer) - numData, dataPtr, dataLength);
	numData += dataLength;
	workBufPtr += dataLength;	//Jump to the checksum

	//calculate the checksum
	checksum = 0;
	for (i = 0; i < numData; i++)
	{
		checksum ^= msgBuffer[i];
	}

	//Add the checksum
	*((uint16_t*)workBufPtr) = checksum;
	numData += 2;
	

	//Start the TX buffer with STX
	txBufPtr = txBuffer;
	*txBufPtr++ = STX;
	totalData++;

	//Copy data and check for reserved symbols and stuff if needed
	for (i = 0; i < numData; i++)
	{
		switch (msgBuffer[i])
		{
		case STX:
		case ETX:
		case DLE:
			//Reserved data, add a stuff byte and stuff the data
			*txBufPtr++ = DLE;
			totalData++;
			*txBufPtr++ = msgBuffer[i] ^ 0x10;
			break;

		default:
			*txBufPtr++ = msgBuffer[i];
			break;
		}
		totalData++;
	}

	//Finish off by adding the ETX
	*txBufPtr++ = ETX;
	totalData++;
	   
	serialSendBuffer(txBuffer, totalData);
}
#endif
