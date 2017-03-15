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

#ifndef WINPORT_SEDIT_H__
#define WINPORT_SEDIT_H__

#include <string>

namespace winport
{
	extern bool sedit(std::string& line, const std::string& find, const std::string& replace);
	extern const string sedit2(const std::string& line, const std::string& find, const std::string& replace);
}

#endif
