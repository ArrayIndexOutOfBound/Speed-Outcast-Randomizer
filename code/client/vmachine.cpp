// vmachine.cpp -- wrapper to fake virtual machine for client

#include "vmachine.h"
#pragma warning (disable : 4514)
/*
==============================================================

VIRTUAL MACHINE

==============================================================
*/
int	VM_Call( int callnum, ... )
{
	if (!cgvm.entryPoint) {
		Com_Printf("VM_Call: cgvm.entryPoint is NULL, call %d ignored\n", callnum);
		return 0;
	}
	return cgvm.entryPoint( (&callnum)[0], (&callnum)[1], (&callnum)[2], (&callnum)[3],
		(&callnum)[4], (&callnum)[5], (&callnum)[6], (&callnum)[7],
		(&callnum)[8],  (&callnum)[9] );
	
}

/*
============
VM_DllSyscall

we pass this to the cgame dll to call back into the client
============
*/
extern int CL_CgameSystemCalls( int *args );
extern int CL_UISystemCalls( int *args );

int VM_DllSyscall( int arg, ... ) {
	if (!cgvm.entryPoint) {
		// If cgame isn't loaded, alarm in the console
		Com_Printf("VM_DllSyscall: cgvm not loaded, forwarding to CL_CgameSystemCalls\n");
		return CL_CgameSystemCalls( &arg );
	}
	return CL_CgameSystemCalls( &arg );
}
