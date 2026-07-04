/*=============================================================================
	CorePrivate.h: Unreal core private header file.
	Copyright 1997 Epic MegaGames, Inc. This software is a trade secret.
=============================================================================*/

/*----------------------------------------------------------------------------
	Core public includes.
----------------------------------------------------------------------------*/

#include "Core.h"

#pragma pack(push,4) // KHG: restore MSVC /Zp4 uniform 4-byte packing on GCC/Clang for LP64 layout parity

/*-----------------------------------------------------------------------------
	UTextBufferFactory.
-----------------------------------------------------------------------------*/

//
// Imports UTextBuffer objects.
//
class CORE_API UTextBufferFactory : public UFactory
{
	DECLARE_CLASS_WITHOUT_CONSTRUCT(UTextBufferFactory,UFactory,0)

	// Constructor.
	UTextBufferFactory();

	// UFactory interface.
	UObject* Create( UClass* Class, UObject* InParent, FName Name, UObject* Context, const char* Type, const char*& Buffer, const char* BufferEnd, FFeedbackContext* Warn=NULL );
};

/*-----------------------------------------------------------------------------
	The End.
-----------------------------------------------------------------------------*/
#pragma pack(pop) // KHG
