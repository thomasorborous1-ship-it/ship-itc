#include <stdio.h>
#include <string.h>

#include "types.h"
#include "console.h"
#include "file.h"
#include "mainloop_debug.h"
#include "mainloop_iop.h"
#include "mainloop_load.h"
#include "mainloop_net.h"
#include "mainloop_shared.h"
#include "mainloop_ui.h"
#include "mainloop.h"

extern "C" {
#include "ps2ip.h"
#include "netplay_ee.h"
}

/* MAINLOOP_NETPORT lives in mainloop_shared.h (included above). */

int _MainLoopNetworkEvent(Uint32 Type, Uint32 Parm1, void *Parm2)
{
    NetPlayRPCStatusT status;
	switch (Type)
	{
		case 1:
            printf("Connecting to %08X\n", Parm1);
            NetPlayClientConnect(Parm1, MAINLOOP_NETPORT);
			break;
		case 2:
            NetPlayGetStatus(&status);
            if (status.eServerStatus == NETPLAY_STATUS_IDLE)
            {
               NetPlayServerStart(MAINLOOP_NETPORT, Parm1);
               NetPlayClientConnect(0x0100007F, MAINLOOP_NETPORT);
           }
           else
           NetPlayServerStop();
			break;
		case 3:
            NetPlayGetStatus(&status);
            if (status.eClientStatus == NETPLAY_STATUS_IDLE)
            {
				return 1;
            } else
            {
                NetPlayClientDisconnect();
				return 0;
            }
			break;
	}

	return 0;
}

void *_MainLoopNetCallback(NetPlayCallbackE eCallback, char *data, int size)
{
    switch (eCallback)
    {
        case NETPLAY_CALLBACK_NONE:
            break;

        case NETPLAY_CALLBACK_CONNECTED:
            printf("NetClientEE: Connected\n");
            break;

        case NETPLAY_CALLBACK_DISCONNECTED:
            printf("NetClientEE: Disconnected\n");
            break;

        case NETPLAY_CALLBACK_LOADGAME:
            {
                Bool result = FALSE;

                printf("NetClientEE: Loading the netgame %s\n", data);
                if (size > 0)
                {
                    //  load here (no-sram)
					result = _MainLoopExecuteFile(data, FALSE);
                }

                if (!result)
                {
                    NetPlayClientSendLoadAck(NETPLAY_LOADACK_ERROR);
                }  else
                {
                    NetPlayClientSendLoadAck(NETPLAY_LOADACK_OK);
                }
            }
            break;

        case NETPLAY_CALLBACK_UNLOADGAME:
            printf("NetClientEE: Unloading the netgame\n");
            _MainLoopUnloadRom();
            break;

        case NETPLAY_CALLBACK_STARTGAME:
            printf("NetClientEE: Starting the netgame\n");
            _MenuEnable(FALSE);
            break;

        default:
            printf("NetClientEE: Callback %d\n", eCallback);
            break;

    }
	return NULL;
}

char *_MainLoop_NetConfigPaths[]=
{
	(char *)"mc0:/SNESticle/",
	_MainLoop_BootDir,
    NULL
};




static Bool _MainLoopLoadNetConfig(t_ip_info *pConfig, const char *pConfigPath)
{
	// 
	printf("netconfigload: %s\n", pConfigPath);
	return FALSE;
}

Bool _MainLoopConfigureNetwork(char **ppSearchPaths, char *pConfigFileName)
{
    t_ip_info config;

	// reset ip configuration
    memset(&config, 0, sizeof(config));

	strcpy(config.netif_name, "sm1");

	// setup default config to have dhcp enabled
	config.dhcp_enabled = 1;
	config.ipaddr.s_addr = 0;
	config.netmask.s_addr = 0;
	config.gw.s_addr = 0;

	// go through all search paths
	while (*ppSearchPaths!=NULL)
	{
		if (strlen(*ppSearchPaths) > 0)
		{
		    char Path[1024];

        	sprintf(Path, "%s%s", *ppSearchPaths, pConfigFileName);

			// attempt to load configuration information
			if (_MainLoopLoadNetConfig(&config, Path))
			{
				// loaded!
				break;
			}
		}
		ppSearchPaths++;
	}

// set configuration
	ps2ip_setconfig(&config);

	if (ps2ip_getconfig(config.netif_name,&config))
	{
		// print info about network configuration
		printf("%08X %08X %08X %d\n", config.ipaddr.s_addr, config.netmask.s_addr, config.gw.s_addr, config.dhcp_enabled);
	}

	return TRUE;
}

Bool _MainLoopInitNetwork(Char **ppSearchPaths)
{
    int ret;
	Bool bLoadedNetwork = FALSE;

	// attempt to load ps2ips (the ps2ip iop rpc server)
	// if it does load, then it means that ps2ip is already set up (from ps2link or some other loader)
	//      and we need not load ps2ip ourselves
    ret = IOPLoadModule("PS2IPS.IRX", ppSearchPaths, 0, NULL);
    if (ret < 0)
    {
		// load ps2ip modules
        IOPLoadModule("PS2IP.IRX", ppSearchPaths, 0, NULL);
        IOPLoadModule("PS2SMAP.IRX", ppSearchPaths, 0, NULL);

        ret = IOPLoadModule("PS2IPS.IRX", ppSearchPaths, 0, NULL);
		if (ret < 0)
		{
			// network not setup
			return FALSE;
		}

		bLoadedNetwork = TRUE;
    }

	// init ps2ip
#if 0	
    printf("ps2ip_Init()\n");
    ps2ip_init();
#endif
	// return TRUE if we initialized networking ourselves
	return bLoadedNetwork;
}
