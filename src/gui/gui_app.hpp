/*
** Copyright (C) 2004 Jesse Chappell <jesse@essej.net>
**  
** This program is free software; you can redistribute it and/or modify
** it under the terms of the GNU General Public License as published by
** the Free Software Foundation; either version 2 of the License, or
** (at your option) any later version.
**  
** This program is distributed in the hope that it will be useful,
** but WITHOUT ANY WARRANTY; without even the implied warranty of
** MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
** GNU General Public License for more details.
**  
** You should have received a copy of the GNU General Public License
** along with this program; if not, write to the Free Software
** Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
**  
*/

#ifndef __sooperlooper_gui_app__
#define __sooperlooper_gui_app__


#include <wx/wx.h>


namespace SooperLooperGui {


class GuiFrame;
	
class GuiApp : public wxApp
{
	
  public: 
	// override base class virtuals
	// ----------------------------
	GuiApp();

	virtual ~GuiApp();
	
	// this one is called on application startup and is a good place for the app
	// initialization (doing it here and not in the ctor allows to have an error
	// return: if OnInit() returns false, the application terminates)
	virtual bool OnInit();
	
	GuiFrame * getFrame() { return _frame; }

	void setupSignals();

	wxString get_host() { return _host; }
	int get_port() { return _port; }

	bool get_force_spawn() { return _force_spawn; }
	wxString get_exec_name() { return _exec_name; }
	char ** get_engine_args () { return _engine_argv; }
	
  protected:
	
	void  usage(char *argv0);
	void  parse_options (int argc, char **argv);

	void process_key_event (wxKeyEvent &ev);

	GuiFrame * _frame;

	
	wxString _host;
	int      _port;
	int _show_usage;
	int _show_version;
	wxString _exec_name;
	bool  _force_spawn;
	char ** _engine_argv;

	DECLARE_EVENT_TABLE()
	
};



};

DECLARE_APP(SooperLooperGui::GuiApp);


#endif