/* Copyright (C) 2014 EryDB, Inc.

   This program is free software; you can redistribute it and/or
   modify it under the terms of the GNU General Public License
   as published by the Free Software Foundation; version 2 of
   the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston,
   MA 02110-1301, USA. */

//  $Id$


#ifndef _UTILS_UTF8_H_
#define _UTILS_UTF8_H_



#include <string>
#if defined(_MSC_VER)
#include <malloc.h>
#include <windows.h>
#elif defined(__FreeBSD__)
//#include <cstdlib>
#else
#include <alloca.h>
#endif
#include <cstdlib>

#include <clocale>

#include "snmpmanager.h"
using namespace snmpmanager;

#include "liboamcpp.h"
using namespace oam;


/** @file */

namespace funcexp
{
namespace utf8
{
extern bool JPcodePoint;		// code point ordering (Japanese UTF) flag, used in erydb_strcoll

const int MAX_UTF8_BYTES_PER_CHAR=4;


//Erydb version of strlocale  BUG 5362
//set System Locale "C" by default
//return the system Locale currently set in from erydb.xml
inline
std::string erydb_setlocale()
{
	// get and set locale language
	std::string systemLang("C");
	Oam oam;
	try
	{
		oam.getSystemConfig("SystemLang", systemLang);
	}
	catch(...)
	{
		systemLang = "C";
	}
	
	char*pLoc = setlocale(LC_ALL, systemLang.c_str());
	if(pLoc == NULL)
	{
		try
		{
			//send alarm
			SNMPManager alarmMgr;
			std::string alarmItem = "system";
			alarmMgr.sendAlarmReport(alarmItem.c_str(), oam::INVALID_LOCALE, SET);
			printf("Failed to set locale : %s, Critical alarm generated\n", systemLang.c_str());
		}
		catch(...)
		{
			// Ignoring for time being.
		}
	}
	else
	{
		try
		{
			//send alarm
			SNMPManager alarmMgr;
			std::string alarmItem = "system";
			alarmMgr.sendAlarmReport(alarmItem.c_str(), oam::INVALID_LOCALE, CLEAR);
		}
		catch(...)
		{
			// Ignoring for time being.
		}

	}

	printf ("Locale is : %s\n", systemLang.c_str() );

	//BUG 2991
	setlocale(LC_NUMERIC, "C");
	if(systemLang.find("ja_JP") != std::string::npos)
		JPcodePoint = true;
	return systemLang;	
}

// Erydb version of strcoll.  BUG 5362
// strcoll() comparison while ja_JP.utf8 does not give correct results.
// For correct results strcmp() can be used.
inline
int erydb_strcoll(const char* str1, const char* str2)
{
	if (JPcodePoint)
		return strcmp(str1, str2);
	else
		return strcoll(str1, str2);
}


// BUG 5241
// Erydb specific mbstowcs(). This will handle both windows and unix platforms
// Params dest and max should have enough length to accomodate NULL
inline
size_t erydb_mbstowcs(wchar_t* dest, const char* src, size_t max)
{
#ifdef _MSC_VER
	// 4th param (-1) denotes to convert till hit NULL char
	// if 6th param max = 0, will return the required buffer size
	size_t strwclen = MultiByteToWideChar(CP_UTF8, 0, src, -1, dest, (int)max);
	// decrement the count of NULL; will become -1 on failure
	return --strwclen;

#else
	return mbstowcs(dest, src, max);
#endif
}

// BUG 5241
// Erydb specific wcstombs(). This will handle both windows and unix platforms
// Params dest and max should have enough length to accomodate NULL
inline
size_t erydb_wcstombs(char* dest, const wchar_t* src, size_t max)
{
#ifdef _MSC_VER
	// 4th param (-1) denotes to convert till hit NULL char
	//if 6th param max = 0, will return the required buffer size 
	size_t strmblen = WideCharToMultiByte( CP_UTF8, 0, src, -1, dest, (int)max, NULL, NULL);
	// decrement the count of NULL; will become -1 on failure
	return --strmblen;
#else
	return wcstombs(dest, src, max);
#endif
}

// convert UTF-8 string to wstring
inline
std::wstring utf8_to_wstring (const std::string& str)
{
    int bufsize = (int)((str.length()+1) * sizeof(wchar_t));

    // Convert to wide characters. Do all further work in wide characters
    wchar_t* wcbuf = (wchar_t*)alloca(bufsize);
	// Passing +1 so that windows is happy to see extra position to place NULL
    size_t strwclen = erydb_mbstowcs(wcbuf, str.c_str(), str.length()+1);
    // if result is -1 it means bad characters which may happen if locale is wrong.
    // return an empty string
    if( strwclen == static_cast<size_t>(-1) )
    	strwclen = 0;
    return std::wstring(wcbuf,strwclen);
}


// convert wstring to UTF-8 string
inline
std::string wstring_to_utf8 (const std::wstring& str)
{
    char* outbuf = (char*)alloca((str.length()*MAX_UTF8_BYTES_PER_CHAR)+1);
	// Passing +1 so that windows is happy to see extra position to place NULL
    size_t strmblen = erydb_wcstombs(outbuf, str.c_str(), str.length()*MAX_UTF8_BYTES_PER_CHAR+1);

    // if result is -1 it means bad characters which may happen if locale is wrong.
    // return an empty string
    if( strmblen == static_cast<size_t>(-1) )
    	strmblen = 0;
    return std::string(outbuf, strmblen);
}

} //namespace utf8
} //namespace funcexp

#endif
