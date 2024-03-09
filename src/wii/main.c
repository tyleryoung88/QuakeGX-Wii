/*
Quake GameCube port.
Copyright (C) 2007 Peter Mackay
Copyright (C) 2008 Eluan Miranda
Copyright (C) 2015 Fabio Olimpieri

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  

See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.
*/

// ELUTODO: I use libfat here, move everything to system_libfat.h and use generic calls here

// Handy switches.
#define CONSOLE_DEBUG		0
#define TIME_DEMO			0
#define USE_THREAD			1
#define TEST_CONNECTION		0
#define USBGECKO_DEBUG		0
#define WIFI_DEBUG			0

// OGC includes.
#include <ogc/lwp.h>
#include <ogc/lwp_mutex.h>
#include <ogc/lwp_watchdog.h>
#include <ogcsys.h>
#include <wiiuse/wpad.h>
#include <wiikeyboard/keyboard.h>
#include <fat.h>
#include "input_wiimote.h"

#if USBGECKO_DEBUG || WIFI_DEBUG
#include <debug.h>
#endif

#include <sys/stat.h>
#include <sys/dir.h>
#include <unistd.h>

#include "../generic/quakedef.h"

u32 MALLOC_MEM2 = 0;

extern void Sys_Reset(void);
extern void Sys_Shutdown(void);

// Video globals.
void		*framebuffer[2]		= {NULL, NULL};
u32		fb			= 0;
GXRModeObj	*rmode			= 0;

int want_to_reset = 0;
int want_to_shutdown = 0;
int texture_memory = 30;
double time_wpad_off = 0;
double current_time = 0;
int rumble_on = 0;

void reset_system(void)
{
	want_to_reset = 1;
}

void shutdown_system(void)
{
	want_to_shutdown = 1;
}

		// Set up the heap.
		static size_t	heap_size	= 19 * 1024 * 1024;
		static char		*heap;

		inline void *align32 (void *p)
		{
			return (void*)((((int)p + 31)) & 0xffffffe0);
		}

		static void init()
		{
			fb = 0;

			// Initialise the video system.
			VIDEO_Init();

			rmode = VIDEO_GetPreferredMode(NULL);

			// Allocate the frame buffer.
			framebuffer[0] = MEM_K0_TO_K1(SYS_AllocateFramebuffer(rmode));
			framebuffer[1] = MEM_K0_TO_K1(SYS_AllocateFramebuffer(rmode));

			// Set up the video system with the chosen mode.
			VIDEO_Configure(rmode);

			// Set the frame buffer.
			VIDEO_SetNextFramebuffer(framebuffer[fb]);

			VIDEO_SetBlack(FALSE);
			VIDEO_Flush();
			VIDEO_WaitVSync();
			if (rmode->viTVMode & VI_NON_INTERLACE)
			{
				VIDEO_WaitVSync();
			}

			// Initialise the debug console.
			// ELUTODO: only one framebuffer with it?
			console_init(framebuffer[0], 20, 20, rmode->fbWidth, rmode->xfbHeight, rmode->fbWidth * 2);

			// Initialise the controller library.
			PAD_Init();

			// Initialise the keyboard library
			KEYBOARD_Init(NULL);

			if(!fatInitDefault())
				printf("Error initializing filesystem\n");
			
			Sys_Init_Logfile();

#ifndef DISABLE_WIIMOTE
			if (WPAD_Init() != WPAD_ERR_NONE)
				Sys_Error("WPAD_Init() failed.\n");
#endif

			wiimote_ir_res_x = rmode->fbWidth;
			wiimote_ir_res_y = rmode->xfbHeight;
		}

		static void check_pak_file_exists()
		{
			int handle = -1;
			char quake_pack[64];
			strcpy(quake_pack, QUAKE_WII_BASEDIR);
			strcat(quake_pack,"/nzp/pak0.pak");
			if (Sys_FileOpenRead(quake_pack, &handle) < 0)
			{
				Sys_Error(
					QUAKE_WII_BASEDIR"/ID1/PAK0.PAK was not found.\n"
					"\n"
					"This file comes with the full or demo version of Quake\n"
					"and is necessary for the game to run.\n"
					"\n"
					"Please make sure it is on your SD card in the correct\n"
					"location.\n"
					"\n"
					"If you are absolutely sure the file is correct, your SD\n"
					"card may not be compatible with the SD card lib which\n"
					"Quake uses, or the Wii. Please check the issue tracker.");
				return;
			}
			else
			{
				Sys_FileClose(handle);
			}
		}

		// ELUTODO: ugly and beyond quake's limits, I think
		int parms_number = 0;
		char parms[1024];
		char *parms_ptr = parms;
		char *parms_array[64];
		static void add_parm(const char *parm)
		{
			if (strlen(parm) + ((u32)parms_ptr - (u32)parms) > 1023)
				Sys_Error("cmdline > 1024");
			
			strcpy(parms_ptr, parm);
			parms_array[parms_number++] = parms_ptr;
			parms_ptr += strlen(parm) + 1;
		}

		char *mods_names[64];
		
		void push_back(char *name, int index)
		{
			int len;
			len = strlen(name);
			mods_names[index]=(char *) malloc(len+1);
			memset(mods_names[index],0,len+1);
			strcpy(mods_names[index], name);
		}
		
		int cstring_cmp(const void *p1, const void *p2)
		{
			return strcmp(*(char * const *)p1, *(char * const *)p2);
		}
		
		void frontend(void)
		{

			u32 cursor = 0;
			u32 cursor_modulus = 6;

			// option 0
			u32 missionpack_selected = 0;
			u32 missionpack_have = 1; // bitmask 1 = standard, 2 = scourge of armagon (hipnotic), 4 = dissolution of eternity (rogue)
			const char *missionpack_names[3] = {"Standard Quake", "Scourge of Armagon", "Dissolution of Eternity" };
			int heap_memory = 19;
			// option 1
			u32 mods_selected = 0;
			
			// option 2
			u32 network_enable = 0;

			// option 3
			u32 listen_players = 4;

			// find mods / mission packs
			push_back("None", 0); 

			DIR *fatdir;
			struct dirent *dir;
			struct stat filestat;
			char filename[128];
			int mod_index=1;

			fatdir = opendir(QUAKE_WII_BASEDIR);
			if (!fatdir)
				Sys_Error("Error opening %s for read.\n", QUAKE_WII_BASEDIR);

			while ((dir = readdir(fatdir)) != NULL)
			{
				sprintf(filename, "%s/%s", QUAKE_WII_BASEDIR,dir->d_name);
				
				if (stat (filename, &filestat)) continue;	
			
				if ((filestat.st_mode & S_IFDIR) && strcmp(dir->d_name, ".") && strcmp(dir->d_name, "..") && strcasecmp(dir->d_name, "nzp"))
				{
					if(!strcasecmp(dir->d_name, "hipnotic"))
						missionpack_have |= 2;
					else if(!strcasecmp(dir->d_name, "rogue"))
						missionpack_have |= 4;
					else
						{if ((mod_index<63)&&(strlen(dir->d_name)<32)); push_back(dir->d_name, mod_index); mod_index++;}
				}
			}
			mods_names[mod_index]= NULL;

			closedir(fatdir);
	
			if (mod_index>2) qsort(mods_names+1, mod_index-1, sizeof(char *), cstring_cmp);

			while (1)
			{
				PAD_ScanPads();
				WPAD_ScanPads();

				u32 gcpress, wmpress;
				gcpress = PAD_ButtonsDown(0) | PAD_ButtonsDown(1) | PAD_ButtonsDown(2) | PAD_ButtonsDown(3);
				wmpress = WPAD_ButtonsDown(0) | WPAD_ButtonsDown(1) | WPAD_ButtonsDown(2) | WPAD_ButtonsDown(3);
				bool up = (gcpress & PAD_BUTTON_UP) | (wmpress & WPAD_BUTTON_UP);
				bool down = (gcpress & PAD_BUTTON_DOWN) | (wmpress & WPAD_BUTTON_DOWN);
				bool left = (gcpress & PAD_BUTTON_LEFT) | (wmpress & WPAD_BUTTON_LEFT);
				bool right = (gcpress & PAD_BUTTON_RIGHT) | (wmpress & WPAD_BUTTON_RIGHT);
				bool start = (gcpress & PAD_BUTTON_START) | (wmpress & WPAD_BUTTON_PLUS);

				printf("\x1b[2;0H");
				// ELUTODO: use CONF module to configure certain settings according to the wii's options

				if (up)
					cursor = (cursor - 1 + cursor_modulus) % cursor_modulus;
				if (down)
					cursor = (cursor + 1) % cursor_modulus;
				if (left)
				{
					switch (cursor)
					{
						case 0:
							missionpack_selected = (missionpack_selected - 1 + 3) % 3;
							break;
						case 1:
							mods_selected = (mods_selected - 1 + mod_index) % mod_index;
							break;
						case 2:
							heap_memory--;
							if (heap_memory < 16)
							heap_memory = 16;
							break;
						case 3:
							texture_memory--;
							if (texture_memory < 16)
							texture_memory = 16;
							break;	
						case 4:
							network_enable = !network_enable;
							break;
						case 5:
							listen_players--;
							if (listen_players < 4)
								listen_players = 4;
							break;
						default:
							Sys_Error("frontend: Invalid cursor position");
							break;
					}
				}
				if (right)
				{
					switch (cursor)
					{
						case 0:
							missionpack_selected = (missionpack_selected + 1) % 3;
							break;
						case 1:
							mods_selected = (mods_selected + 1) % mod_index;
							break;
						case 2:
							if ((heap_memory < 35)&&(heap_memory+texture_memory<51))
							heap_memory++;
							break;
						case 3:
							if ((texture_memory < 35)&&(heap_memory+texture_memory<51))
							texture_memory++;
							break;	
						case 4:
							network_enable = !network_enable;
							break;
						case 5:
							listen_players++;
							if (listen_players > 16)
								listen_players = 16;
							break;
						default:
							Sys_Error("frontend: Invalid cursor position");
							break;
					}
				}


				if (start)
				{
					printf("\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n     Starting Quake...\n\n\n\n");
					break;
				}

				printf("\n\n     Press UP, DOWN, LEFT, RIGHT to make your selections\n");
				printf("     Press START/PLUS to start the game\n\n");

				printf("     %c Mission Pack:      %s%s\n", cursor == 0 ? '>' : ' ', missionpack_names[missionpack_selected],
					(missionpack_have & (1 << missionpack_selected)) ? "                              " : " (You don't have it!)         ");

				const u32 mods_maxprintsize = 32;
				char mods_printvar[mods_maxprintsize];
				strncpy(mods_printvar, mods_names[mods_selected], mods_maxprintsize);
				size_t mods_printsize = strlen(mods_printvar);
				u32 i;

				//fill the string with spaces at the end of the name
				for (i = mods_printsize; i < mods_maxprintsize - 1; i++)
					mods_printvar[i] = ' ';
				mods_printvar[i] = '\0';

				printf("     %c Mod:               %s\n", cursor == 1 ? '>' : ' ', mods_printvar);
						
				printf("     %c Heap Memory:       %d MB\n", cursor == 2 ? '>' : ' ', heap_memory);
				
				printf("     %c Texture Memory:    %d MB\n", cursor == 3 ? '>' : ' ', texture_memory);
				
				printf("     %c Enable Network:    %s\n", cursor == 4 ? '>' : ' ', network_enable ? "yes" : "no ");

				printf("     %c Max Network Slots: %u   \n", cursor == 5 ? '>' : ' ', listen_players);

				printf("\n\n\n     Network is experimental, may fail randomly.\n     Please activate it to use bots.\n");

				VIDEO_WaitVSync();

			}

			// Initialise the Common module.
			heap_size	= heap_memory * 1024 * 1024;
			
			add_parm("Quake");
#if CONSOLE_DEBUG
			add_parm("-condebug");
#endif
			if (missionpack_have & (1 << missionpack_selected))
			{
				switch(missionpack_selected)
				{
					case 0:
						break;
					case 1:
						add_parm("-hipnotic");
						break;
					case 2:
						add_parm("-rogue");
						break;
					default:
						Sys_Error("frontend: Invalid mission pack selected");
						break;
				}
			}

			if (mods_selected)
			{
				add_parm("-game");
				add_parm(mods_names[mods_selected]); // ELUTODO: bad thing to do?
			}
			
			for(mod_index=0;  mods_names[mod_index]!= NULL; mod_index++)
				free(mods_names[mod_index]);

			if (!network_enable)
			{
				add_parm("-noudp");
			}
			else
			{
				char temp_num[32];
				snprintf(temp_num, 32, "%u", listen_players);
				add_parm("-listen");
				add_parm(temp_num);
			}
		}

		static void* main_thread_function(void* dummy)
		{
			u32 level, real_heap_size;

			// hope the parms are all set by now
			COM_InitArgv(parms_number, parms_array);

			_CPU_ISR_Disable(level);
			heap = (char *)align32(SYS_GetArena2Lo());
			real_heap_size = heap_size - ((u32)heap - (u32)SYS_GetArena2Lo());
			if ((u32)heap + real_heap_size > (u32)SYS_GetArena2Hi())
			{
				_CPU_ISR_Restore(level);
				Sys_Error("heap + real_heap_size > (u32)SYS_GetArena2Hi()");
			}	
			else
			{
				SYS_SetArena2Lo(heap + real_heap_size);
				_CPU_ISR_Restore(level);
			}

			VIDEO_SetBlack(TRUE);

			// Initialise the Host module.
			quakeparms_t parms;
			memset(&parms, 0, sizeof(parms));
			parms.argc		= com_argc;
			parms.argv		= com_argv;
			parms.basedir	= QUAKE_WII_BASEDIR;
			parms.memsize	= real_heap_size;
			parms.membase	= heap;
			if (parms.membase == 0)
			{
				Sys_Error("Heap allocation failed");
			}
			memset(parms.membase, 0, parms.memsize);
			Host_Init(&parms);

#if TIME_DEMO
			Cbuf_AddText("map start\n");
			Cbuf_AddText("wait\n");
			Cbuf_AddText("timedemo demo1\n");
#endif
#if TEST_CONNECTION
			Cbuf_AddText("connect 192.168.0.2");
#endif

			SYS_SetResetCallback(reset_system);
			SYS_SetPowerCallback(shutdown_system);

			VIDEO_SetBlack(FALSE);

			// Run the main loop.
			double current_time, last_time, seconds;
			
			last_time = Sys_FloatTime ();
			
			for (;;)
			{
				if (want_to_reset)
					Sys_Reset();
				if (want_to_shutdown)
					Sys_Shutdown();

				// Get the frame time in ticks.
				current_time = Sys_FloatTime ();
				seconds	= current_time - last_time;
				last_time = current_time;
				
				//Con_Printf ("time: %f \n", current_time_millisec);
				//Con_Printf ("time off: %f \n", time_wpad_off_millisec);
				
				if (rumble_on&&(current_time > time_wpad_off)) 
				{
					WPAD_Rumble(0, FALSE);
					rumble_on = 0;
				}

				// Run the frame.
				Host_Frame(seconds);
			};

			// Quit (this code is never reached).
			Sys_Quit();
			return 0;
		}


qboolean isDedicated = FALSE;

int main(int argc, char* argv[])
{
	void *qstack = malloc(4 * 1024 * 1024); // ELUTODO: clean code to prevent needing a stack this huge

#if USBGECKO_DEBUG
	DEBUG_Init(GDBSTUB_DEVICE_USB, 1); // Slot B
	_break();
#endif

#if WIFI_DEBUG
	printf("Now waiting for WI-FI debugger\n");
	DEBUG_Init(GDBSTUB_DEVICE_WIFI, 8000); // Port 8000 (use whatever you want)
	_break();
#endif


	// Initialize.
	init();
	check_pak_file_exists();

	frontend();

	// Start the main thread.
	lwp_t thread;
	LWP_CreateThread(&thread, &main_thread_function, 0, qstack, 4 * 1024 * 1024, 64);

	// Wait for it to finish.
	void* result;
	LWP_JoinThread(thread, &result);

	exit(0);
	return 0;
}
