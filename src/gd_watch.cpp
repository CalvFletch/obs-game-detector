#define _WIN32_DCOM
#define WIN32_LEAN_AND_MEAN
#include "gd_api.h"

#include <obs-module.h>

#include <windows.h>
#include <tlhelp32.h>
#include <comdef.h>
#include <wbemidl.h>

#include <stdio.h>
#include <string.h>

#pragma comment(lib, "wbemuuid.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")

typedef struct {
	DWORD pid;
	HANDLE process;
	HANDLE wait;
	char exe[GD_MAX_PATH];
	bool active;
} exit_watch_t;

typedef struct {
	DWORD pid;
	char exe[GD_MAX_PATH];
	char path[GD_MAX_PATH];
} proc_entry_t;

#define SNAPSHOT_CAP 512
static proc_entry_t s_snapshot_procs[SNAPSHOT_CAP];

static IWbemServices *s_wmi_svc = NULL;
static HANDLE s_wmi_thread = NULL;
static HANDLE s_wmi_stop = NULL;
static exit_watch_t s_exits[GD_MAX_GAMES];
static CRITICAL_SECTION s_exit_lock;
static bool s_exit_lock_init = false;

static void exit_lock_init(void)
{
	if (s_exit_lock_init)
		return;
	InitializeCriticalSection(&s_exit_lock);
	s_exit_lock_init = true;
}

static void exit_lock_fini(void)
{
	if (!s_exit_lock_init)
		return;
	DeleteCriticalSection(&s_exit_lock);
	s_exit_lock_init = false;
}

// wmi Indicate/wait callbacks run on worker threads; only enqueue here
// obs work happens in processor_tick on the ui thread
static void post_start(DWORD pid, const char *exe_lower, const char *full_path)
{
	GD_State *state = gd_state();
	if (!state || !state->post_event)
		return;

	GD_Event evt = {};
	evt.kind = GD_EVT_PROCESS_START;
	evt.pid = pid;
	gd_strlcpy(evt.exe_lower, exe_lower, sizeof(evt.exe_lower));
	gd_strlcpy(evt.full_path, full_path ? full_path : "", sizeof(evt.full_path));
	state->post_event(&evt);
}

static void post_stop(DWORD pid, const char *exe_lower)
{
	GD_State *state = gd_state();
	if (!state || !state->post_event)
		return;

	GD_Event evt = {};
	evt.kind = GD_EVT_PROCESS_STOP;
	evt.pid = pid;
	gd_strlcpy(evt.exe_lower, exe_lower, sizeof(evt.exe_lower));
	state->post_event(&evt);
}

static exit_watch_t *exit_slot_for_pid(DWORD pid)
{
	for (int i = 0; i < GD_MAX_GAMES; i++) {
		if (s_exits[i].active && s_exits[i].pid == pid)
			return &s_exits[i];
	}
	return NULL;
}

static exit_watch_t *exit_slot_alloc(void)
{
	for (int i = 0; i < GD_MAX_GAMES; i++) {
		if (!s_exits[i].active)
			return &s_exits[i];
	}
	return NULL;
}

static void exit_slot_clear(exit_watch_t *slot)
{
	if (!slot || !slot->active)
		return;

	if (slot->wait) {
		UnregisterWaitEx(slot->wait, INVALID_HANDLE_VALUE);
		slot->wait = NULL;
	}
	if (slot->process) {
		CloseHandle(slot->process);
		slot->process = NULL;
	}
	slot->pid = 0;
	slot->exe[0] = '\0';
	slot->active = false;
}

static VOID CALLBACK on_process_exit(PVOID ctx, BOOLEAN timed_out)
{
	(void)timed_out;

	exit_watch_t *slot = (exit_watch_t *)ctx;
	if (!slot || !slot->active)
		return;

	DWORD pid = slot->pid;
	char exe[GD_MAX_PATH];
	gd_strlcpy(exe, slot->exe, sizeof(exe));
	post_stop(pid, exe);
}

void gd_watch_disarm_exit(DWORD pid)
{
	if (!pid || !s_exit_lock_init)
		return;

	EnterCriticalSection(&s_exit_lock);
	exit_watch_t *slot = exit_slot_for_pid(pid);
	if (slot)
		exit_slot_clear(slot);
	LeaveCriticalSection(&s_exit_lock);
}

static void disarm_all_exits(void)
{
	if (!s_exit_lock_init)
		return;

	EnterCriticalSection(&s_exit_lock);
	for (int i = 0; i < GD_MAX_GAMES; i++) {
		if (s_exits[i].active)
			exit_slot_clear(&s_exits[i]);
	}
	LeaveCriticalSection(&s_exit_lock);
}

void gd_watch_arm_exit(DWORD pid, const char *exe_lower)
{
	if (!pid || !exe_lower || !exe_lower[0])
		return;

	exit_lock_init();

	gd_watch_disarm_exit(pid);

	HANDLE hp = OpenProcess(SYNCHRONIZE, FALSE, pid);
	if (!hp) {
		blog(LOG_DEBUG, "[obs-game-detector] exit wait: pid %lu already gone", (unsigned long)pid);
		post_stop(pid, exe_lower);
		return;
	}

	EnterCriticalSection(&s_exit_lock);

	exit_watch_t *slot = exit_slot_alloc();
	if (!slot) {
		LeaveCriticalSection(&s_exit_lock);
		CloseHandle(hp);
		blog(LOG_ERROR, "[obs-game-detector] exit wait table full, pid %lu untracked", (unsigned long)pid);
		return;
	}

	slot->pid = pid;
	slot->process = hp;
	slot->wait = NULL;
	gd_strlcpy(slot->exe, exe_lower, sizeof(slot->exe));
	slot->active = true;

	if (!RegisterWaitForSingleObject(&slot->wait, hp, on_process_exit, slot, INFINITE, WT_EXECUTEONLYONCE)) {
		blog(LOG_ERROR, "[obs-game-detector] RegisterWaitForSingleObject failed for pid %lu",
		     (unsigned long)pid);
		exit_slot_clear(slot);
		LeaveCriticalSection(&s_exit_lock);
		return;
	}

	LeaveCriticalSection(&s_exit_lock);
}

static int scan_processes(proc_entry_t *out, int cap)
{
	HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
	if (snap == INVALID_HANDLE_VALUE)
		return 0;

	PROCESSENTRY32 pe;
	pe.dwSize = sizeof(pe);
	int count = 0;

	if (Process32First(snap, &pe)) {
		do {
			if (count >= cap)
				break;
			proc_entry_t *e = &out[count];
			e->pid = pe.th32ProcessID;
			e->exe[0] = '\0';
			e->path[0] = '\0';

			HANDLE hp = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pe.th32ProcessID);
			if (hp) {
				DWORD sz = (DWORD)sizeof(e->path) - 1;
				QueryFullProcessImageNameA(hp, 0, e->path, &sz);
				e->path[sz] = '\0';
				CloseHandle(hp);
			}
			if (e->path[0]) {
				const char *fn = strrchr(e->path, '\\');
				fn = fn ? fn + 1 : e->path;
				gd_strlower(e->exe, fn, sizeof(e->exe));
				count++;
			}
		} while (Process32Next(snap, &pe));
	}
	CloseHandle(snap);
	return count;
}

static bool get_target_process(IWbemClassObject *event, IWbemClassObject **out_proc)
{
	VARIANT vt;
	VariantInit(&vt);
	if (FAILED(event->Get(L"TargetInstance", 0, &vt, NULL, NULL)))
		return false;
	if (vt.vt != VT_UNKNOWN || !vt.punkVal) {
		VariantClear(&vt);
		return false;
	}

	HRESULT hr = vt.punkVal->QueryInterface(IID_IWbemClassObject, (void **)out_proc);
	VariantClear(&vt);
	return SUCCEEDED(hr);
}

static bool variant_to_pid(const VARIANT *v, DWORD *pid_out)
{
	switch (v->vt) {
	case VT_I4:
		*pid_out = (DWORD)v->lVal;
		return true;
	case VT_UI4:
		*pid_out = (DWORD)v->ulVal;
		return true;
	case VT_BSTR:
		*pid_out = (DWORD)wcstoul(v->bstrVal, NULL, 10);
		return true;
	default:
		return false;
	}
}

/* Read pid + exe name from a Win32_ProcessStartTrace extrinsic event.
 * These properties (ProcessID, ProcessName) live directly on the event. */
static bool read_trace_event(IWbemClassObject *event, char *exe_lower, size_t exe_cap, DWORD *pid_out)
{
	DWORD pid = 0;
	VARIANT v;

	VariantInit(&v);
	if (FAILED(event->Get(L"ProcessID", 0, &v, NULL, NULL)) || !variant_to_pid(&v, &pid)) {
		VariantClear(&v);
		return false;
	}
	VariantClear(&v);

	VariantInit(&v);
	if (FAILED(event->Get(L"ProcessName", 0, &v, NULL, NULL)) || v.vt != VT_BSTR) {
		VariantClear(&v);
		return false;
	}
	WideCharToMultiByte(CP_ACP, 0, v.bstrVal, -1, exe_lower, (int)exe_cap, NULL, NULL);
	VariantClear(&v);

	for (char *p = exe_lower; *p; p++)
		*p = (char)tolower((unsigned char)*p);

	if (pid_out)
		*pid_out = pid;
	return exe_lower[0] != '\0';
}

/* Read pid + exe name from an intrinsic __InstanceCreationEvent (fallback path
 * for environments where the trace provider isn't available). */
static bool read_intrinsic_event(IWbemClassObject *event, char *exe_lower, size_t exe_cap, DWORD *pid_out)
{
	IWbemClassObject *proc = NULL;
	if (!get_target_process(event, &proc))
		return false;

	DWORD pid = 0;
	VARIANT v;
	VariantInit(&v);
	if (SUCCEEDED(proc->Get(L"ProcessId", 0, &v, NULL, NULL)))
		variant_to_pid(&v, &pid);
	VariantClear(&v);

	VariantInit(&v);
	if (FAILED(proc->Get(L"Name", 0, &v, NULL, NULL)) || v.vt != VT_BSTR) {
		proc->Release();
		VariantClear(&v);
		return false;
	}

	WideCharToMultiByte(CP_ACP, 0, v.bstrVal, -1, exe_lower, (int)exe_cap, NULL, NULL);
	VariantClear(&v);
	proc->Release();

	for (char *p = exe_lower; *p; p++)
		*p = (char)tolower((unsigned char)*p);

	if (pid_out)
		*pid_out = pid;
	return exe_lower[0] != '\0';
}

/* Try the trace event first; fall back to intrinsic parsing. */
static bool read_process_event(IWbemClassObject *event, char *exe_lower, size_t exe_cap, DWORD *pid_out)
{
	if (read_trace_event(event, exe_lower, exe_cap, pid_out))
		return true;
	return read_intrinsic_event(event, exe_lower, exe_cap, pid_out);
}

class ProcessCreationSink : public IWbemObjectSink {
	LONG m_ref;

public:
	ProcessCreationSink() : m_ref(1) {}

	ULONG STDMETHODCALLTYPE AddRef() override { return (ULONG)InterlockedIncrement(&m_ref); }

	ULONG STDMETHODCALLTYPE Release() override
	{
		InterlockedDecrement(&m_ref);
		return (ULONG)m_ref;
	}

	HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppv) override
	{
		if (riid == IID_IUnknown || riid == IID_IWbemObjectSink) {
			*ppv = static_cast<IWbemObjectSink *>(this);
			AddRef();
			return S_OK;
		}
		*ppv = NULL;
		return E_NOINTERFACE;
	}

	HRESULT STDMETHODCALLTYPE Indicate(LONG count, IWbemClassObject **objs) override
	{
		for (LONG i = 0; i < count; i++) {
			char exe_lower[GD_MAX_PATH];
			DWORD pid = 0;

			if (!read_process_event(objs[i], exe_lower, sizeof(exe_lower), &pid))
				continue;

			char full_path[GD_MAX_PATH] = {0};
			if (pid) {
				HANDLE hp = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
				if (hp) {
					DWORD sz = GD_MAX_PATH - 1;
					QueryFullProcessImageNameA(hp, 0, full_path, &sz);
					full_path[sz] = '\0';
					CloseHandle(hp);
				}
			}
			post_start(pid, exe_lower, full_path);
		}
		return WBEM_S_NO_ERROR;
	}

	HRESULT STDMETHODCALLTYPE SetStatus(LONG, HRESULT, BSTR, IWbemClassObject *) override
	{
		return WBEM_S_NO_ERROR;
	}
};

static ProcessCreationSink g_creation_sink;

static bool setup_wmi(void)
{
	IWbemLocator *pLoc = NULL;
	HRESULT hr = CoCreateInstance(CLSID_WbemLocator, NULL, CLSCTX_INPROC_SERVER, IID_IWbemLocator, (LPVOID *)&pLoc);
	if (FAILED(hr))
		return false;

	hr = pLoc->ConnectServer(_bstr_t(L"ROOT\\CIMV2"), NULL, NULL, NULL, 0, NULL, NULL, &s_wmi_svc);
	pLoc->Release();
	if (FAILED(hr))
		return false;

	hr = CoSetProxyBlanket(s_wmi_svc, RPC_C_AUTHN_WINNT, RPC_C_AUTHZ_NONE, NULL, RPC_C_AUTHN_LEVEL_CALL,
			       RPC_C_IMP_LEVEL_IMPERSONATE, NULL, EOAC_NONE);
	if (FAILED(hr)) {
		s_wmi_svc->Release();
		s_wmi_svc = NULL;
		return false;
	}

	BSTR lang = SysAllocString(L"WQL");

	/* Preferred: Win32_ProcessStartTrace is an extrinsic ETW-backed event that
	 * fires the instant a process starts. No WITHIN clause => true push, no
	 * repository polling. Requires the host process to be elevated. */
	BSTR trace_query = SysAllocString(L"SELECT * FROM Win32_ProcessStartTrace");
	hr = s_wmi_svc->ExecNotificationQueryAsync(lang, trace_query, WBEM_FLAG_SEND_STATUS, NULL, &g_creation_sink);
	SysFreeString(trace_query);

	if (FAILED(hr)) {
		/* Fallback for non-elevated hosts: intrinsic creation event. This
		 * polls the CIM repository on the WITHIN interval. */
		BSTR intrinsic_query = SysAllocString(L"SELECT * FROM __InstanceCreationEvent WITHIN 1 "
						      L"WHERE TargetInstance ISA 'Win32_Process'");
		hr = s_wmi_svc->ExecNotificationQueryAsync(lang, intrinsic_query, WBEM_FLAG_SEND_STATUS, NULL,
							   &g_creation_sink);
		SysFreeString(intrinsic_query);
	}

	SysFreeString(lang);

	if (FAILED(hr)) {
		g_creation_sink.Release();
		return false;
	}

	return true;
}

static void teardown_wmi(void)
{
	if (s_wmi_svc) {
		s_wmi_svc->CancelAsyncCall(&g_creation_sink);
		g_creation_sink.Release();
		s_wmi_svc->Release();
		s_wmi_svc = NULL;
	}
}

static DWORD WINAPI wmi_thread_func(LPVOID)
{
	HRESULT hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
	bool own_com = SUCCEEDED(hr);

	if (SUCCEEDED(hr) || hr == (HRESULT)RPC_E_CHANGED_MODE) {
		if (setup_wmi()) {
			blog(LOG_INFO, "[obs-game-detector] WMI process creation watch active");
		} else {
			blog(LOG_ERROR, "[obs-game-detector] WMI process creation watch failed");
		}
	} else {
		blog(LOG_ERROR, "[obs-game-detector] WMI thread COM init failed (0x%08lx)", (unsigned long)hr);
	}

	WaitForSingleObject(s_wmi_stop, INFINITE);
	teardown_wmi();
	if (own_com)
		CoUninitialize();
	return 0;
}

void gd_watch_snapshot(const GD_LookupTable *lt)
{
	int n = scan_processes(s_snapshot_procs, SNAPSHOT_CAP);
	if (n == 0) {
		blog(LOG_ERROR, "[obs-game-detector] boot snapshot: process scan failed");
		return;
	}

	int hits = 0;
	for (int i = 0; i < n; i++) {
		if (lt && !gd_lookup_matches_full_path(lt, s_snapshot_procs[i].path))
			continue;
		hits++;
		post_start(s_snapshot_procs[i].pid, s_snapshot_procs[i].exe, s_snapshot_procs[i].path);
	}

	blog(LOG_INFO, "[obs-game-detector] boot snapshot: %d lookup hits / %d with paths", hits, n);
}

bool gd_watch_start(void)
{
	exit_lock_init();
	memset(s_exits, 0, sizeof(s_exits));

	s_wmi_stop = CreateEvent(NULL, TRUE, FALSE, NULL);
	s_wmi_thread = CreateThread(NULL, 0, wmi_thread_func, NULL, 0, NULL);
	return s_wmi_thread != NULL;
}

void gd_watch_stop(void)
{
	disarm_all_exits();

	if (s_wmi_stop)
		SetEvent(s_wmi_stop);
	if (s_wmi_thread) {
		WaitForSingleObject(s_wmi_thread, 10000);
		CloseHandle(s_wmi_thread);
		s_wmi_thread = NULL;
	}
	if (s_wmi_stop) {
		CloseHandle(s_wmi_stop);
		s_wmi_stop = NULL;
	}

	exit_lock_fini();
}
