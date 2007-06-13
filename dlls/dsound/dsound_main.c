/*  			DirectSound
 *
 * Copyright 1998 Marcus Meissner
 * Copyright 1998 Rob Riggs
 * Copyright 2000-2002 TransGaming Technologies, Inc.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */
/*
 * Most thread locking is complete. There may be a few race
 * conditions still lurking.
 *
 * Tested with a Soundblaster clone, a Gravis UltraSound Classic,
 * and a Turtle Beach Tropez+.
 *
 * TODO:
 *	Implement SetCooperativeLevel properly (need to address focus issues)
 *	Implement DirectSound3DBuffers (stubs in place)
 *	Use hardware 3D support if available
 *      Add critical section locking inside Release and AddRef methods
 *      Handle static buffers - put those in hardware, non-static not in hardware
 *      Hardware DuplicateSoundBuffer
 *      Proper volume calculation, and setting volume in HEL primary buffer
 *      Optimize WINMM and negotiate fragment size, decrease DS_HEL_MARGIN
 */

#include <stdarg.h>

#define COBJMACROS
#define NONAMELESSSTRUCT
#define NONAMELESSUNION
#include "windef.h"
#include "winbase.h"
#include "winuser.h"
#include "winnls.h"
#include "winreg.h"
#include "mmsystem.h"
#include "winternl.h"
#include "mmddk.h"
#include "wine/debug.h"
#include "dsound.h"
#include "dsdriver.h"
#include "dsound_private.h"
#include "dsconf.h"

WINE_DEFAULT_DEBUG_CHANNEL(dsound);

/* these are eligible for tuning... they must be high on slow machines... */
/* some stuff may get more responsive with lower values though... */
#define DS_EMULDRIVER 0 /* some games (Quake 2, UT) refuse to accept
				emulated dsound devices. set to 0 ! */
#define DS_HEL_MARGIN 5 /* HEL only: number of waveOut fragments ahead to mix in new buffers
			 * (keep this close or equal to DS_HEL_QUEUE for best results) */
#define DS_HEL_QUEUE  5 /* HEL only: number of waveOut fragments ahead to queue to driver
			 * (this will affect HEL sound reliability and latency) */

#define DS_SND_QUEUE_MAX 10 /* max number of fragments to prebuffer */

DirectSoundDevice*	DSOUND_renderer[MAXWAVEDRIVERS];
GUID                    DSOUND_renderer_guids[MAXWAVEDRIVERS];
GUID                    DSOUND_capture_guids[MAXWAVEDRIVERS];

HRESULT mmErr(UINT err)
{
	switch(err) {
	case MMSYSERR_NOERROR:
		return DS_OK;
	case MMSYSERR_ALLOCATED:
		return DSERR_ALLOCATED;
	case MMSYSERR_ERROR:
	case MMSYSERR_INVALHANDLE:
	case WAVERR_STILLPLAYING:
		return DSERR_GENERIC; /* FIXME */
	case MMSYSERR_NODRIVER:
		return DSERR_NODRIVER;
	case MMSYSERR_NOMEM:
		return DSERR_OUTOFMEMORY;
	case MMSYSERR_INVALPARAM:
	case WAVERR_BADFORMAT:
	case WAVERR_UNPREPARED:
		return DSERR_INVALIDPARAM;
	case MMSYSERR_NOTSUPPORTED:
		return DSERR_UNSUPPORTED;
	default:
		FIXME("Unknown MMSYS error %d\n",err);
		return DSERR_GENERIC;
	}
}

int ds_emuldriver = DS_EMULDRIVER;
int ds_hel_margin = DS_HEL_MARGIN;
int ds_hel_queue = DS_HEL_QUEUE;
int ds_snd_queue_max = DS_SND_QUEUE_MAX;
int ds_hw_accel = DS_HW_ACCEL_FULL;
int ds_default_playback = 0;
int ds_default_capture = 0;
int ds_default_sample_rate = 22050;
int ds_default_bits_per_sample = 8;

/*
 * Get a config key from either the app-specific or the default config
 */

static inline DWORD get_config_key( HKEY defkey, HKEY appkey, const char *name,
                                    char *buffer, DWORD size )
{
    if (appkey && !RegQueryValueExA( appkey, name, 0, NULL, (LPBYTE)buffer, &size )) return 0;
    if (defkey && !RegQueryValueExA( defkey, name, 0, NULL, (LPBYTE)buffer, &size )) return 0;
    return ERROR_FILE_NOT_FOUND;
}


/*
 * Setup the dsound options.
 */

void setup_dsound_options(void)
{
    char buffer[MAX_PATH+16];
    HKEY hkey, appkey = 0;
    DWORD len;

    buffer[MAX_PATH]='\0';

    /* @@ Wine registry key: HKCU\Software\Wine\DirectSound */
    if (RegOpenKeyA( HKEY_CURRENT_USER, "Software\\Wine\\DirectSound", &hkey )) hkey = 0;

    len = GetModuleFileNameA( 0, buffer, MAX_PATH );
    if (len && len < MAX_PATH)
    {
        HKEY tmpkey;
        /* @@ Wine registry key: HKCU\Software\Wine\AppDefaults\app.exe\DirectSound */
        if (!RegOpenKeyA( HKEY_CURRENT_USER, "Software\\Wine\\AppDefaults", &tmpkey ))
        {
            char *p, *appname = buffer;
            if ((p = strrchr( appname, '/' ))) appname = p + 1;
            if ((p = strrchr( appname, '\\' ))) appname = p + 1;
            strcat( appname, "\\DirectSound" );
            TRACE("appname = [%s]\n", appname);
            if (RegOpenKeyA( tmpkey, appname, &appkey )) appkey = 0;
            RegCloseKey( tmpkey );
        }
    }

    /* get options */

    if (!get_config_key( hkey, appkey, "EmulDriver", buffer, MAX_PATH ))
        ds_emuldriver = strcmp(buffer, "N");

    if (!get_config_key( hkey, appkey, "HELmargin", buffer, MAX_PATH ))
        ds_hel_margin = atoi(buffer);

    if (!get_config_key( hkey, appkey, "HELqueue", buffer, MAX_PATH ))
        ds_hel_queue = atoi(buffer);

    if (!get_config_key( hkey, appkey, "SndQueueMax", buffer, MAX_PATH ))
        ds_snd_queue_max = atoi(buffer);

    if (!get_config_key( hkey, appkey, "HardwareAcceleration", buffer, MAX_PATH )) {
	if (strcmp(buffer, "Full") == 0)
	    ds_hw_accel = DS_HW_ACCEL_FULL;
	else if (strcmp(buffer, "Standard") == 0)
	    ds_hw_accel = DS_HW_ACCEL_STANDARD;
	else if (strcmp(buffer, "Basic") == 0)
	    ds_hw_accel = DS_HW_ACCEL_BASIC;
	else if (strcmp(buffer, "Emulation") == 0)
	    ds_hw_accel = DS_HW_ACCEL_EMULATION;
    }

    if (!get_config_key( hkey, appkey, "DefaultPlayback", buffer, MAX_PATH ))
	    ds_default_playback = atoi(buffer);

    if (!get_config_key( hkey, appkey, "DefaultCapture", buffer, MAX_PATH ))
	    ds_default_capture = atoi(buffer);

    if (!get_config_key( hkey, appkey, "DefaultSampleRate", buffer, MAX_PATH ))
	    ds_default_sample_rate = atoi(buffer);

    if (!get_config_key( hkey, appkey, "DefaultBitsPerSample", buffer, MAX_PATH ))
	    ds_default_bits_per_sample = atoi(buffer);

    if (appkey) RegCloseKey( appkey );
    if (hkey) RegCloseKey( hkey );

    if (ds_emuldriver != DS_EMULDRIVER )
       WARN("ds_emuldriver = %d (default=%d)\n",ds_emuldriver, DS_EMULDRIVER);
    if (ds_hel_margin != DS_HEL_MARGIN )
       WARN("ds_hel_margin = %d (default=%d)\n",ds_hel_margin, DS_HEL_MARGIN );
    if (ds_hel_queue != DS_HEL_QUEUE )
       WARN("ds_hel_queue = %d (default=%d)\n",ds_hel_queue, DS_HEL_QUEUE );
    if (ds_snd_queue_max != DS_SND_QUEUE_MAX)
       WARN("ds_snd_queue_max = %d (default=%d)\n",ds_snd_queue_max ,DS_SND_QUEUE_MAX);
    if (ds_hw_accel != DS_HW_ACCEL_FULL)
	WARN("ds_hw_accel = %s (default=Full)\n",
	    ds_hw_accel==DS_HW_ACCEL_FULL ? "Full" :
	    ds_hw_accel==DS_HW_ACCEL_STANDARD ? "Standard" :
	    ds_hw_accel==DS_HW_ACCEL_BASIC ? "Basic" :
	    ds_hw_accel==DS_HW_ACCEL_EMULATION ? "Emulation" :
	    "Unknown");
    if (ds_default_playback != 0)
	WARN("ds_default_playback = %d (default=0)\n",ds_default_playback);
    if (ds_default_capture != 0)
	WARN("ds_default_capture = %d (default=0)\n",ds_default_playback);
    if (ds_default_sample_rate != 22050)
        WARN("ds_default_sample_rate = %d (default=22050)\n",ds_default_sample_rate);
    if (ds_default_bits_per_sample != 8)
        WARN("ds_default_bits_per_sample = %d (default=8)\n",ds_default_bits_per_sample);
}

static const char * get_device_id(LPCGUID pGuid)
{
    if (IsEqualGUID(&DSDEVID_DefaultPlayback, pGuid))
        return "DSDEVID_DefaultPlayback";
    else if (IsEqualGUID(&DSDEVID_DefaultVoicePlayback, pGuid))
        return "DSDEVID_DefaultVoicePlayback";
    else if (IsEqualGUID(&DSDEVID_DefaultCapture, pGuid))
        return "DSDEVID_DefaultCapture";
    else if (IsEqualGUID(&DSDEVID_DefaultVoiceCapture, pGuid))
        return "DSDEVID_DefaultVoiceCapture";
    return debugstr_guid(pGuid);
}

/***************************************************************************
 * GetDeviceID	[DSOUND.9]
 *
 * Retrieves unique identifier of default device specified
 *
 * PARAMS
 *    pGuidSrc  [I] Address of device GUID.
 *    pGuidDest [O] Address to receive unique device GUID.
 *
 * RETURNS
 *    Success: DS_OK
 *    Failure: DSERR_INVALIDPARAM
 *
 * NOTES
 *    pGuidSrc is a valid device GUID or DSDEVID_DefaultPlayback,
 *    DSDEVID_DefaultCapture, DSDEVID_DefaultVoicePlayback, or
 *    DSDEVID_DefaultVoiceCapture.
 *    Returns pGuidSrc if pGuidSrc is a valid device or the device
 *    GUID for the specified constants.
 */
HRESULT WINAPI GetDeviceID(LPCGUID pGuidSrc, LPGUID pGuidDest)
{
    TRACE("(%s,%p)\n", get_device_id(pGuidSrc),pGuidDest);

    if ( pGuidSrc == NULL) {
	WARN("invalid parameter: pGuidSrc == NULL\n");
	return DSERR_INVALIDPARAM;
    }

    if ( pGuidDest == NULL ) {
	WARN("invalid parameter: pGuidDest == NULL\n");
	return DSERR_INVALIDPARAM;
    }

    if ( IsEqualGUID( &DSDEVID_DefaultPlayback, pGuidSrc ) ||
    	 IsEqualGUID( &DSDEVID_DefaultVoicePlayback, pGuidSrc ) ) {
	CopyMemory(pGuidDest, &DSOUND_renderer_guids[ds_default_playback], sizeof(GUID));
        TRACE("returns %s\n", get_device_id(pGuidDest));
	return DS_OK;
    }

    if ( IsEqualGUID( &DSDEVID_DefaultCapture, pGuidSrc ) ||
    	 IsEqualGUID( &DSDEVID_DefaultVoiceCapture, pGuidSrc ) ) {
	CopyMemory(pGuidDest, &DSOUND_capture_guids[ds_default_capture], sizeof(GUID));
        TRACE("returns %s\n", get_device_id(pGuidDest));
	return DS_OK;
    }

    CopyMemory(pGuidDest, pGuidSrc, sizeof(GUID));
    TRACE("returns %s\n", get_device_id(pGuidDest));

    return DS_OK;
}


/***************************************************************************
 * DirectSoundEnumerateA [DSOUND.2]
 *
 * Enumerate all DirectSound drivers installed in the system
 *
 * PARAMS
 *    lpDSEnumCallback  [I] Address of callback function.
 *    lpContext         [I] Address of user defined context passed to callback function.
 *
 * RETURNS
 *    Success: DS_OK
 *    Failure: DSERR_INVALIDPARAM
 */
HRESULT WINAPI DirectSoundEnumerateA(
    LPDSENUMCALLBACKA lpDSEnumCallback,
    LPVOID lpContext)
{
    unsigned devs, wod;
    DSDRIVERDESC desc;
    GUID guid;
    int err;

    TRACE("lpDSEnumCallback = %p, lpContext = %p\n",
	lpDSEnumCallback, lpContext);

    if (lpDSEnumCallback == NULL) {
	WARN("invalid parameter: lpDSEnumCallback == NULL\n");
	return DSERR_INVALIDPARAM;
    }

    devs = waveOutGetNumDevs();
    if (devs > 0) {
	if (GetDeviceID(&DSDEVID_DefaultPlayback, &guid) == DS_OK) {
	    for (wod = 0; wod < devs; ++wod) {
                if (IsEqualGUID( &guid, &DSOUND_renderer_guids[wod]) ) {
                    err = mmErr(waveOutMessage((HWAVEOUT)wod,DRV_QUERYDSOUNDDESC,(DWORD_PTR)&desc,0));
                    if (err == DS_OK) {
                        TRACE("calling lpDSEnumCallback(NULL,\"%s\",\"%s\",%p)\n",
                              "Primary Sound Driver",desc.szDrvname,lpContext);
                        if (lpDSEnumCallback(NULL, "Primary Sound Driver", desc.szDrvname, lpContext) == FALSE)
                            return DS_OK;
		    }
		}
	    }
	}
    }

    for (wod = 0; wod < devs; ++wod) {
	err = mmErr(waveOutMessage((HWAVEOUT)wod,DRV_QUERYDSOUNDDESC,(DWORD_PTR)&desc,0));
	if (err == DS_OK) {
            TRACE("calling lpDSEnumCallback(%s,\"%s\",\"%s\",%p)\n",
                  debugstr_guid(&DSOUND_renderer_guids[wod]),desc.szDesc,desc.szDrvname,lpContext);
            if (lpDSEnumCallback(&DSOUND_renderer_guids[wod], desc.szDesc, desc.szDrvname, lpContext) == FALSE)
                return DS_OK;
	}
    }
    return DS_OK;
}

/***************************************************************************
 * DirectSoundEnumerateW [DSOUND.3]
 *
 * Enumerate all DirectSound drivers installed in the system
 *
 * PARAMS
 *    lpDSEnumCallback  [I] Address of callback function.
 *    lpContext         [I] Address of user defined context passed to callback function.
 *
 * RETURNS
 *    Success: DS_OK
 *    Failure: DSERR_INVALIDPARAM
 */
HRESULT WINAPI DirectSoundEnumerateW(
	LPDSENUMCALLBACKW lpDSEnumCallback,
	LPVOID lpContext )
{
    unsigned devs, wod;
    DSDRIVERDESC desc;
    GUID guid;
    int err;
    WCHAR wDesc[MAXPNAMELEN];
    WCHAR wName[MAXPNAMELEN];

    TRACE("lpDSEnumCallback = %p, lpContext = %p\n",
	lpDSEnumCallback, lpContext);

    if (lpDSEnumCallback == NULL) {
	WARN("invalid parameter: lpDSEnumCallback == NULL\n");
	return DSERR_INVALIDPARAM;
    }

    devs = waveOutGetNumDevs();
    if (devs > 0) {
	if (GetDeviceID(&DSDEVID_DefaultPlayback, &guid) == DS_OK) {
	    for (wod = 0; wod < devs; ++wod) {
                if (IsEqualGUID( &guid, &DSOUND_renderer_guids[wod] ) ) {
                    err = mmErr(waveOutMessage((HWAVEOUT)wod,DRV_QUERYDSOUNDDESC,(DWORD_PTR)&desc,0));
                    if (err == DS_OK) {
                        TRACE("calling lpDSEnumCallback(NULL,\"%s\",\"%s\",%p)\n",
                              "Primary Sound Driver",desc.szDrvname,lpContext);
                        MultiByteToWideChar( CP_ACP, 0, "Primary Sound Driver", -1,
                                             wDesc, sizeof(wDesc)/sizeof(WCHAR) );
                        MultiByteToWideChar( CP_ACP, 0, desc.szDrvname, -1,
                                             wName, sizeof(wName)/sizeof(WCHAR) );
                        if (lpDSEnumCallback(NULL, wDesc, wName, lpContext) == FALSE)
                            return DS_OK;
		    }
		}
	    }
	}
    }

    for (wod = 0; wod < devs; ++wod) {
	err = mmErr(waveOutMessage((HWAVEOUT)wod,DRV_QUERYDSOUNDDESC,(DWORD_PTR)&desc,0));
	if (err == DS_OK) {
            TRACE("calling lpDSEnumCallback(%s,\"%s\",\"%s\",%p)\n",
                  debugstr_guid(&DSOUND_renderer_guids[wod]),desc.szDesc,desc.szDrvname,lpContext);
            MultiByteToWideChar( CP_ACP, 0, desc.szDesc, -1,
                                 wDesc, sizeof(wDesc)/sizeof(WCHAR) );
            MultiByteToWideChar( CP_ACP, 0, desc.szDrvname, -1,
                                 wName, sizeof(wName)/sizeof(WCHAR) );
            if (lpDSEnumCallback(&DSOUND_renderer_guids[wod], wDesc, wName, lpContext) == FALSE)
                return DS_OK;
	}
    }
    return DS_OK;
}

/*******************************************************************************
 * DirectSound ClassFactory
 */

typedef  HRESULT (*FnCreateInstance)(REFIID riid, LPVOID *ppobj);
 
typedef struct {
    const IClassFactoryVtbl *lpVtbl;
    LONG ref;
    REFCLSID rclsid;
    FnCreateInstance pfnCreateInstance;
} IClassFactoryImpl;

static HRESULT WINAPI
DSCF_QueryInterface(LPCLASSFACTORY iface, REFIID riid, LPVOID *ppobj)
{
    IClassFactoryImpl *This = (IClassFactoryImpl *)iface;
    TRACE("(%p, %s, %p)\n", This, debugstr_guid(riid), ppobj);
    if (ppobj == NULL)
        return E_POINTER;
    if (IsEqualIID(riid, &IID_IUnknown) ||
        IsEqualIID(riid, &IID_IClassFactory))
    {
        *ppobj = iface;
        IUnknown_AddRef(iface);
        return S_OK;
    }
    *ppobj = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI DSCF_AddRef(LPCLASSFACTORY iface)
{
    IClassFactoryImpl *This = (IClassFactoryImpl *)iface;
    ULONG ref = InterlockedIncrement(&(This->ref));
    TRACE("(%p) ref was %d\n", This, ref - 1);
    return ref;
}

static ULONG WINAPI DSCF_Release(LPCLASSFACTORY iface)
{
    IClassFactoryImpl *This = (IClassFactoryImpl *)iface;
    ULONG ref = InterlockedDecrement(&(This->ref));
    TRACE("(%p) ref was %d\n", This, ref + 1);
    /* static class, won't be freed */
    return ref;
}

static HRESULT WINAPI DSCF_CreateInstance(
    LPCLASSFACTORY iface,
    LPUNKNOWN pOuter,
    REFIID riid,
    LPVOID *ppobj)
{
    IClassFactoryImpl *This = (IClassFactoryImpl *)iface;
    TRACE("(%p, %p, %s, %p)\n", This, pOuter, debugstr_guid(riid), ppobj);

    if (pOuter)
        return CLASS_E_NOAGGREGATION;

    if (ppobj == NULL) {
        WARN("invalid parameter\n");
        return DSERR_INVALIDPARAM;
    }
    *ppobj = NULL;
    return This->pfnCreateInstance(riid, ppobj);
}
 
static HRESULT WINAPI DSCF_LockServer(LPCLASSFACTORY iface, BOOL dolock)
{
    IClassFactoryImpl *This = (IClassFactoryImpl *)iface;
    FIXME("(%p, %d) stub!\n", This, dolock);
    return S_OK;
}

static const IClassFactoryVtbl DSCF_Vtbl = {
    DSCF_QueryInterface,
    DSCF_AddRef,
    DSCF_Release,
    DSCF_CreateInstance,
    DSCF_LockServer
};

static IClassFactoryImpl DSOUND_CF[] = {
    { &DSCF_Vtbl, 1, &CLSID_DirectSound, (FnCreateInstance)DSOUND_Create },
    { &DSCF_Vtbl, 1, &CLSID_DirectSound8, (FnCreateInstance)DSOUND_Create8 },
    { &DSCF_Vtbl, 1, &CLSID_DirectSoundCapture, (FnCreateInstance)DSOUND_CaptureCreate },
    { &DSCF_Vtbl, 1, &CLSID_DirectSoundCapture8, (FnCreateInstance)DSOUND_CaptureCreate8 },
    { &DSCF_Vtbl, 1, &CLSID_DirectSoundFullDuplex, (FnCreateInstance)DSOUND_FullDuplexCreate },
    { &DSCF_Vtbl, 1, &CLSID_DirectSoundPrivate, (FnCreateInstance)IKsPrivatePropertySetImpl_Create },
    { NULL, 0, NULL, NULL }
};

/*******************************************************************************
 * DllGetClassObject [DSOUND.@]
 * Retrieves class object from a DLL object
 *
 * NOTES
 *    Docs say returns STDAPI
 *
 * PARAMS
 *    rclsid [I] CLSID for the class object
 *    riid   [I] Reference to identifier of interface for class object
 *    ppv    [O] Address of variable to receive interface pointer for riid
 *
 * RETURNS
 *    Success: S_OK
 *    Failure: CLASS_E_CLASSNOTAVAILABLE, E_OUTOFMEMORY, E_INVALIDARG,
 *             E_UNEXPECTED
 */
HRESULT WINAPI DllGetClassObject(REFCLSID rclsid, REFIID riid, LPVOID *ppv)
{
    int i = 0;
    TRACE("(%s, %s, %p)\n", debugstr_guid(rclsid), debugstr_guid(riid), ppv);

    if (ppv == NULL) {
        WARN("invalid parameter\n");
        return E_INVALIDARG;
    }

    *ppv = NULL;

    if (!IsEqualIID(riid, &IID_IClassFactory) &&
        !IsEqualIID(riid, &IID_IUnknown)) {
        WARN("no interface for %s\n", debugstr_guid(riid));
        return E_NOINTERFACE;
    }

    while (NULL != DSOUND_CF[i].rclsid) {
        if (IsEqualGUID(rclsid, DSOUND_CF[i].rclsid)) {
            DSCF_AddRef((IClassFactory*) &DSOUND_CF[i]);
            *ppv = &DSOUND_CF[i];
            return S_OK;
        }
        i++;
    }

    WARN("(%s, %s, %p): no class found.\n", debugstr_guid(rclsid),
         debugstr_guid(riid), ppv);
    return CLASS_E_CLASSNOTAVAILABLE;
}


/*******************************************************************************
 * DllCanUnloadNow [DSOUND.4]
 * Determines whether the DLL is in use.
 *
 * RETURNS
 *    Success: S_OK
 *    Failure: S_FALSE
 */
HRESULT WINAPI DllCanUnloadNow(void)
{
    FIXME("(void): stub\n");
    return S_FALSE;
}

#define INIT_GUID(guid, l, w1, w2, b1, b2, b3, b4, b5, b6, b7, b8)      \
        guid.Data1 = l; guid.Data2 = w1; guid.Data3 = w2;               \
        guid.Data4[0] = b1; guid.Data4[1] = b2; guid.Data4[2] = b3;     \
        guid.Data4[3] = b4; guid.Data4[4] = b5; guid.Data4[5] = b6;     \
        guid.Data4[6] = b7; guid.Data4[7] = b8;

/***********************************************************************
 *           DllMain (DSOUND.init)
 */
BOOL WINAPI DllMain(HINSTANCE hInstDLL, DWORD fdwReason, LPVOID lpvReserved)
{
    int i;
    TRACE("(%p %d %p)\n", hInstDLL, fdwReason, lpvReserved);

    switch (fdwReason) {
    case DLL_PROCESS_ATTACH:
        TRACE("DLL_PROCESS_ATTACH\n");
        for (i = 0; i < MAXWAVEDRIVERS; i++) {
            DSOUND_renderer[i] = NULL;
            DSOUND_capture[i] = NULL;
            INIT_GUID(DSOUND_renderer_guids[i], 0xbd6dd71a, 0x3deb, 0x11d1, 0xb1, 0x71, 0x00, 0xc0, 0x4f, 0xc2, 0x00, 0x00 + i);
            INIT_GUID(DSOUND_capture_guids[i],  0xbd6dd71b, 0x3deb, 0x11d1, 0xb1, 0x71, 0x00, 0xc0, 0x4f, 0xc2, 0x00, 0x00 + i);
        }
        break;
    case DLL_PROCESS_DETACH:
        TRACE("DLL_PROCESS_DETACH\n");
        break;
    case DLL_THREAD_ATTACH:
        TRACE("DLL_THREAD_ATTACH\n");
        break;
    case DLL_THREAD_DETACH:
        TRACE("DLL_THREAD_DETACH\n");
        break;
    default:
        TRACE("UNKNOWN REASON\n");
        break;
    }
    return TRUE;
}
