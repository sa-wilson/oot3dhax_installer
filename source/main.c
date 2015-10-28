#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <3ds.h>

#include "filesystem.h"
#include "firmware.h"

char status[256];

Result FSUSER_ControlArchive(Handle handle, FS_archive archive)
{
	u32* cmdbuf=getThreadCommandBuffer();

	u32 b1 = 0, b2 = 0;

	cmdbuf[0]=0x080d0144;
	cmdbuf[1]=archive.handleLow;
	cmdbuf[2]=archive.handleHigh;
	cmdbuf[3]=0x0;
	cmdbuf[4]=0x1; //buffer1 size
	cmdbuf[5]=0x1; //buffer1 size
	cmdbuf[6]=0x1a;
	cmdbuf[7]=(u32)&b1;
	cmdbuf[8]=0x1c;
	cmdbuf[9]=(u32)&b2;

	Result ret=0;
	if((ret=svcSendSyncRequest(handle)))return ret;

	return cmdbuf[1];
}

Result write_savedata(char* path, u8* data, u32 size)
{
	if(!path || !data || !size)return -1;

	Handle outFileHandle;
	u32 bytesWritten;
	Result ret = 0;
	int fail = 0;

	ret = FSUSER_OpenFile(&saveGameFsHandle, &outFileHandle, saveGameArchive, FS_makePath(PATH_CHAR, path), FS_OPEN_CREATE | FS_OPEN_WRITE, FS_ATTRIBUTE_NONE);
	if(ret){fail = -8; goto writeFail;}

	ret = FSFILE_Write(outFileHandle, &bytesWritten, 0x0, data, size, 0x10001);
	if(ret){fail = -9; goto writeFail;}

	ret = FSFILE_Close(outFileHandle);
	if(ret){fail = -10; goto writeFail;}

	ret = FSUSER_ControlArchive(saveGameFsHandle, saveGameArchive);

writeFail:
	if(fail)sprintf(status, "Failed to write to file: %d\n     %08X %08X", fail, (unsigned int)ret, (unsigned int)bytesWritten);
	else sprintf(status, "Successfully wrote to file!\n     %08X               ", (unsigned int)bytesWritten);

	return ret;
}

typedef enum
{
	STATE_NONE,
	STATE_INITIAL,
	STATE_SELECT_SLOT,
	STATE_SELECT_FIRMWARE,
	STATE_DOWNLOAD_PAYLOAD,
	STATE_INSTALL_PAYLOAD,
	STATE_INSTALLED_PAYLOAD,
	STATE_ERROR,
}state_t;

Result http_getredirection(char *url, char *out, u32 out_size)
{
	Result ret=0;
	httpcContext context;

	ret = httpcOpenContext(&context, url, 0);
	if(ret!=0)return ret;


	ret = httpcAddRequestHeaderField(&context, "User-Agent", "oot3dhax");
	if(!ret) ret = httpcBeginRequest(&context);
	if(ret!=0)
	{
		httpcCloseContext(&context);
		return ret;
	}

	ret = httpcGetResponseHeader(&context, "Location", out, out_size);

	httpcCloseContext(&context);

	return 0;
}

Result http_download(httpcContext *context, u8** out_buf, u32* out_size)
{
	Result ret=0;
	u32 statuscode=0;
	u32 contentsize=0;
	u8 *buf;

	ret = httpcBeginRequest(context);
	if(ret!=0)return ret;

	ret = httpcGetResponseStatusCode(context, &statuscode, 0);
	if(ret!=0)return ret;

	if(statuscode!=200)return -2;

	ret=httpcGetDownloadSizeState(context, NULL, &contentsize);
	if(ret!=0)return ret;

	buf = (u8*)malloc(contentsize);
	if(buf==NULL)return -1;
	memset(buf, 0, contentsize);

	ret = httpcDownloadData(context, buf, contentsize, NULL);
	if(ret!=0)
	{
		free(buf);
		return ret;
	}

	if(out_size)*out_size = contentsize;
	if(out_buf)*out_buf = buf;
	else free(buf);

	return 0;
}

u8* save_buffer = NULL;
off_t save_size;

int read_payload(char* path, u8* data)
{
	FILE *file = fopen(path,"rb");
	if (file == NULL)
		return 1;

	// seek to end of file
	fseek(file,0,SEEK_END);

	// file pointer tells us the size
	save_size = ftell(file);

	// seek back to start
	fseek(file,0,SEEK_SET);

	//allocate a buffer
	save_buffer=malloc(save_size);
	if(!save_buffer)
		return 1;

	//read contents !
	off_t bytesRead = fread(save_buffer,1,save_size,file);

	//close the file because we like being nice and tidy
	fclose(file);

	if(save_size!=bytesRead)
		return 1;
	return 0;
}


int main()
{
	httpcInit();

	gfxInitDefault();
	gfxSet3D(false);

	filesystemInit();

	PrintConsole topConsole, bttmConsole;
	consoleInit(GFX_TOP, &topConsole);
	consoleInit(GFX_BOTTOM, &bttmConsole);

	consoleSelect(&topConsole);
	consoleClear();

	state_t current_state = STATE_NONE;
	state_t next_state = STATE_INITIAL;

	static char top_text[2048];
	top_text[0] = '\0';

	int selected_slot = 0;

	int firmware_version[firmware_length] = {0, 0, 9, 0, 0};
	int firmware_selected_value = 0;

	u8* payload_buf = NULL;
	u32 payload_size = 0;

	while (aptMainLoop())
	{
		hidScanInput();
		if(hidKeysDown() & KEY_START)break;

		// transition function
		if(next_state != current_state)
		{
			switch(next_state)
			{
				case STATE_INITIAL:
					strcat(top_text, " Welcome to the oot3dhax installer! Please proceedwith caution, as you might lose data if you don't.You may press START at any time to return to menu.\n                            Press A to continue.\n\n");
					break;
				case STATE_SELECT_SLOT:
					strcat(top_text, " Please select the savegame slot oot3dhax will be\ninstalled to. D-Pad to select, A to continue.\n");
					break;
				case STATE_SELECT_FIRMWARE:
					strcat(top_text, "\n\n\n Please select your console's firmware version.\nOnly select NEW 3DS if you own a New 3DS (XL).\nD-Pad to select, A to continue.\n");
					break;
				case STATE_DOWNLOAD_PAYLOAD:
					sprintf(top_text, "%s\n\n\n Downloading payload...\n", top_text);
					break;
				case STATE_INSTALL_PAYLOAD:
					strcat(top_text, " Installing payload...\n");
					break;
				case STATE_INSTALLED_PAYLOAD:
					strcat(top_text, " Done! oot3dhax was successfully installed.");
					break;
				case STATE_ERROR:
					strcat(top_text, " Looks like something went wrong. :(\n");
					break;
				default:
					break;
			}
			current_state = next_state;
		}

		consoleSelect(&topConsole);
		printf("\x1b[0;%dHoot3dhax installer", (50 - 17) / 2);
		printf("\x1b[1;%dHby smea, yellows8, phase, and meladroit\n\n\n", (50 - 38) / 2);
		printf(top_text);

		// state function
		switch(current_state)
		{
			case STATE_INITIAL:
				if(hidKeysDown() & KEY_A)next_state = STATE_SELECT_SLOT;
				break;
			case STATE_SELECT_SLOT:
				{
					if(hidKeysDown() & KEY_UP)selected_slot++;
					if(hidKeysDown() & KEY_DOWN)selected_slot--;
					if(hidKeysDown() & KEY_A)next_state = STATE_SELECT_FIRMWARE;

					if(selected_slot < 0) selected_slot = 0;
					if(selected_slot > 2) selected_slot = 2;

					printf((selected_slot >= 2) ? "                                             \n" : "                                            ^\n");
					printf("                            Selected slot: %d  \n", selected_slot + 1);
					printf((!selected_slot) ? "                                             \n" : "                                            v\n");
				}
				break;
			case STATE_SELECT_FIRMWARE:
				{
					if(hidKeysDown() & KEY_LEFT)firmware_selected_value--;
					if(hidKeysDown() & KEY_RIGHT)firmware_selected_value++;

					if(firmware_selected_value < 0) firmware_selected_value = 0;
					if(firmware_selected_value >= firmware_length) firmware_selected_value = firmware_length - 1;

					if(hidKeysDown() & KEY_UP)firmware_version[firmware_selected_value]++;
					if(hidKeysDown() & KEY_DOWN)firmware_version[firmware_selected_value]--;

					if(firmware_version[firmware_selected_value] < 0) firmware_version[firmware_selected_value] = 0;
					if(firmware_version[firmware_selected_value] >= firmware_num_values[firmware_selected_value]) firmware_version[firmware_selected_value] = firmware_num_values[firmware_selected_value] - 1;

					if(hidKeysDown() & KEY_A)next_state = STATE_DOWNLOAD_PAYLOAD;

					int offset = 28 + firmware_format_offsets[firmware_selected_value];
					printf((firmware_version[firmware_selected_value] < firmware_num_values[firmware_selected_value] - 1) ? "%*s^%*s" : "%*s-%*s", offset, " ", 50 - offset - 1, " ");
					printf("        Selected firmware: " "%s %s-%s-%s %s" "\n", firmware_labels[0][firmware_version[0]], firmware_labels[1][firmware_version[1]], firmware_labels[2][firmware_version[2]], firmware_labels[3][firmware_version[3]], firmware_labels[4][firmware_version[4]]);
					printf((firmware_version[firmware_selected_value] > 0) ? "%*sv%*s" : "%*s-%*s", offset, " ", 50 - offset - 1, " ");
				}
				break;
			case STATE_DOWNLOAD_PAYLOAD:
				{
					httpcContext context;
					static char in_url[512];
					static char out_url[512];
					sprintf(in_url, "http://smea.mtheall.com/get_payload.php?version=%s-%s-%s-%s-0-%s", firmware_labels_url[0][firmware_version[0]], firmware_labels_url[1][firmware_version[1]], firmware_labels_url[2][firmware_version[2]], firmware_labels_url[3][firmware_version[3]], firmware_labels_url[4][firmware_version[4]]);

					Result ret = http_getredirection(in_url, out_url, 512);
					if(ret)
					{
						sprintf(status, "Failed to grab payload url\n    Error code : %08X", (unsigned int)ret);
						next_state = STATE_ERROR;
						break;
					}

					ret = httpcOpenContext(&context, out_url, 0);
					if(ret)
					{
						sprintf(status, "Failed to open http context\n    Error code: %08X", (unsigned int)ret);
						next_state = STATE_ERROR;
						break;
					}

					ret = http_download(&context, &payload_buf, &payload_size);
					if(ret)
					{
						sprintf(status, "Failed to download payload\n    Error code: %08X", (unsigned int)ret);
						next_state = STATE_ERROR;
						break;
					}

					next_state = STATE_INSTALL_PAYLOAD;
				}
				break;
			case STATE_INSTALL_PAYLOAD:
				{
					static char filename[128];

					sprintf(filename, "save0x.bin.%s", firmware_labels[4][firmware_version[4]]);
					read_payload(filename, save_buffer);
					sprintf(filename, "/save0%d.bin", selected_slot);
					Result ret = write_savedata(filename, save_buffer, save_size);
					if(ret)
					{
						sprintf(status, "Failed to install %s.\n    Error code: %08X", filename, (unsigned int)ret);
						next_state = STATE_ERROR;
						break;
					}
				}

				{
					// delete file
					FSUSER_DeleteFile(&saveGameFsHandle, saveGameArchive, FS_makePath(PATH_CHAR, "/payload.bin"));

					FSUSER_ControlArchive(saveGameFsHandle, saveGameArchive);
				}

				{
					Result ret = write_savedata("/payload.bin", payload_buf, payload_size);
					if(ret)
					{
						sprintf(status, "Failed to install payload\n    Error code : %08X", (unsigned int)ret);
						next_state = STATE_ERROR;
						break;
					}

					next_state = STATE_INSTALLED_PAYLOAD;
				}
				break;
			case STATE_INSTALLED_PAYLOAD:
				next_state = STATE_NONE;
				break;
			default:
				break;
		}

		consoleSelect(&bttmConsole);
		printf("\x1b[0;0H  \n Found a bug? Go to\n    https://github.com/meladroit/oot3dhax_installer/ \n\n  Current status:\n    %s\n", status);

		gspWaitForVBlank();
	}

	filesystemExit();

	gfxExit();
	httpcExit();
	return 0;
}
