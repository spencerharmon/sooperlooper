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


#include "midi_bridge.hpp"

#include <sys/poll.h>
#include <unistd.h>
#include <fcntl.h>

#include <iostream>
#include <cstdio>
#include <cmath>

#include <midi++/parser.h>
#include <midi++/factory.h>


using namespace SooperLooper;
using namespace std;
using namespace MIDI;

// Convert a value in dB's to a coefficent
#define DB_CO(g) ((g) > -90.0f ? powf(10.0f, (g) * 0.05f) : 0.0f)
#define CO_DB(v) (20.0f * log10f(v))

static inline double 
gain_to_uniform_position (double g)
{
	if (g == 0) return 0;
	// this one maxes at 6 dB
	//return pow((6.0*log(g)/log(2.0)+192.0)/198.0, 8.0);

	// this one maxes out at 0 db
	return pow((6.0*log(g)/log(2.0)+198.0)/198.0, 8.0);

}

static inline double 
uniform_position_to_gain (double pos)
{
	if (pos == 0) {
		return 0.0;
	}
	/* XXX Marcus writes: this doesn't seem right to me. but i don't have a better answer ... */
	// this one maxes at 6 dB
	//return pow (2.0,(sqrt(sqrt(sqrt(pos)))*198.0-192.0)/6.0);

	// this one maxes out at 0 db
	return pow (2.0,(sqrt(sqrt(sqrt(pos)))*198.0-198.0)/6.0);
}


MidiBridge::MidiBridge (string name, string oscurl, PortRequest & req)
	: _name (name), _oscurl(oscurl)
{
	_port = 0;
	_done = false;
	
	_addr = lo_address_new_from_url (_oscurl.c_str());
	if (lo_address_errno (_addr) < 0) {
		fprintf(stderr, "MidiBridge:: addr error %d: %s\n", lo_address_errno(_addr), lo_address_errstr(_addr));
	}


	_typemap["cc"] = 0xb0;
	_typemap["n"] = 0x90;
	_typemap["pc"] = 0xc0;

	// temp bindings
// 	_bindings[0x9000 | 48] = EventInfo("note", "record", -1);
// 	_bindings[0x9000 | 49] = EventInfo("note", "undo", -1);
// 	_bindings[0x9000 | 50] = EventInfo("note", "overdub", -1);
// 	_bindings[0x9000 | 51] = EventInfo("note", "redo", -1);
// 	_bindings[0x9000 | 52] = EventInfo("note", "multiply", -1);

// 	_bindings[0x9000 | 54] = EventInfo("note", "mute", -1);
// 	_bindings[0x9000 | 55] = EventInfo("note", "scratch", -1);
// 	_bindings[0x9000 | 56] = EventInfo("note", "reverse", -1);


// 	_bindings[0xb000 | 1] =  EventInfo("set", "feedback", -1, 0.0, 1.0);
// 	_bindings[0xb000 | 7] =  EventInfo("set", "scratch_pos", -1, 0.0, 1.0);
// 	_bindings[0xb000 | 74] =  EventInfo("set", "dry", -1, 0.0, 1.0);
// 	_bindings[0xb000 | 71] =  EventInfo("set", "wet", -1, 0.0, 1.0);

	PortFactory factory;
	
	if ((_port = factory.create_port (req)) == 0) {
		return;
	}

	// this is a callback that will be made from the parser
	_port->input()->any.connect (slot (*this, &MidiBridge::incoming_midi));

	init_thread();
	
}

MidiBridge::~MidiBridge()
{
	lo_address_free (_addr);

	// the port will be freed by the subclass
}


bool
MidiBridge::init_thread()
{

	if (pipe (_midi_request_pipe)) {
		cerr << "Cannot create midi request signal pipe" <<  strerror (errno) << endl;
		return false;
	}

	if (fcntl (_midi_request_pipe[0], F_SETFL, O_NONBLOCK)) {
		cerr << "UI: cannot set O_NONBLOCK on signal read pipe " << strerror (errno) << endl;
		return false;
	}

	if (fcntl (_midi_request_pipe[1], F_SETFL, O_NONBLOCK)) {
		cerr << "UI: cannot set O_NONBLOCK on signal write pipe " << strerror (errno) << endl;
		return false;
	}
	
	pthread_create (&_midi_thread, NULL, &MidiBridge::_midi_receiver, this);
	if (!_midi_thread) {
		return false;
	}
	
	pthread_detach(_midi_thread);

	return true;
}

void
MidiBridge::clear_bindings ()
{
	_bindings.clear();
}

FILE *
MidiBridge::search_open_file (std::string filename)
{
	FILE *bindfile = 0;
	
	if ((bindfile = fopen(filename.c_str(), "r")) == NULL) {
		cerr << "error: could not open " << filename << endl;
	}

	// todo: look for it in systemwide and ~/.sooperlooper/bindings/

	return bindfile;
}


bool
MidiBridge::load_bindings (std::string filename)
{
	//  ch:cmds:param   type  ctrl  instance  [min_val_bound max_val_bound valstyle]
	// cmds is one of:  'pc' = program change  'cc' = control change  'n' = note on/off

	// cmd is { pc = 0xc0 , cc = 0xb0 , on = 0x80 , n = 0x90  }
	// chcmd = cmd + ch
	// lookup key = (chcmd << 8) | param
	// valstyle can be 'gain'
	
	FILE * bindfile = 0;
	char  line[200];

	char cmds[10];
	int chan;
	int param;
	char type[32];
	int typei;
	char ctrl[32];
	char stylestr[32];
	EventInfo::Style style = EventInfo::NormalStyle;
	int  instance = -1;
	float lbound = 0.0f;
	float ubound = 1.0f;
	int key;
	int ret;

	if ((bindfile = search_open_file(filename)) == NULL) {
		cerr << "error: could not open " << filename << endl;
		return false;
	}

	while (fgets (line, sizeof(line), bindfile) != NULL)
	{
		instance = -1;
		lbound = 0.0f;
		ubound = 1.0f;
		stylestr[0] = '\0';
		style = EventInfo::NormalStyle;
		
		// ignore empty lines and # lines
		if (line[0] == '\n' || line[0] == '#') {
			continue;
		}
		
		if ((ret=sscanf (line, "%d %3s %d  %30s  %30s  %d  %f  %f  %6s",
			    &chan, cmds, &param, type, ctrl, &instance, &lbound, &ubound, stylestr) < 5)) {
			cerr << "ret: " << ret << " invalid input line: " << line;
			continue;
		}

		if (_typemap.find(cmds) == _typemap.end()) {
			cerr << "invalid midi type str: " << cmds;
			continue;
		}

		typei = _typemap[cmds];
		key = ((typei + chan) << 8) | param;

		if (stylestr[0] == 'g') {
			style = EventInfo::GainStyle;
		}

		if (_bindings.find(key) == _bindings.end()) {
			_bindings.insert (BindingsMap::value_type ( key, EventList()));
		}
		
		_bindings[key].push_back (EventInfo(type, ctrl, instance, lbound, ubound, style));
		cerr << "added binding: " << type << "  "  << ctrl << "  " << instance << "  " << lbound << "  " << ubound << endl;
	}

	fclose(bindfile);

	return true;
}

void
MidiBridge::incoming_midi (Parser &p, byte *msg, size_t len)
{
	/* we only respond to channel messages */

	byte b1 = msg[0];
	byte b2 = len>1 ? msg[1]: 0;
	byte b3 = len>2 ? msg[2]: 0;

// 	if ((msg[0] & 0xF0) < 0x80 || (msg[0] & 0xF0) > 0xE0) {
// 		return;
// 	}

	queue_midi (b1, b2, b3);
}


void
MidiBridge::queue_midi (MIDI::byte chcmd, MIDI::byte param, MIDI::byte val)
{
	// convert midi to lookup key
	// lookup key = (chcmd << 8) | param

	int key = (chcmd << 8) | param;

	BindingsMap::iterator iter = _bindings.find(key);
	
	if (iter != _bindings.end())
	{
		EventList & elist = (*iter).second;

		for (EventList::iterator eiter = elist.begin(); eiter != elist.end(); ++eiter) {
			EventInfo & info = (*eiter);
			float scaled_val;
			
			if (info.style == EventInfo::GainStyle) {
				scaled_val = (float) ((val/127.0f) *  ( info.ubound - info.lbound)) + info.lbound;
				scaled_val = uniform_position_to_gain (scaled_val);
			}
			else {
				scaled_val = (float) ((val/127.0f) *  ( info.ubound - info.lbound)) + info.lbound;
			}
			// cerr << "found binding: val is " << val << "  scaled: " << scaled_val << endl;
			
			send_osc (info, scaled_val);
		}
	}
	else if (chcmd == MIDI::start) {  // MIDI start
		lo_send(_addr, "/sl/midi_start", "");
	}
	else if (chcmd == MIDI::stop) { // MIDI stop
		lo_send(_addr, "/sl/midi_stop", "");
	}
	else if (chcmd == MIDI::timing) {  // MIDI clock tick
		lo_send(_addr, "/sl/midi_tick", "");
	}
	else {
		// fprintf(stderr, "binding %x not found\n", key);
	}
}


void
MidiBridge::send_osc (EventInfo & info, float val)
{
	static char tmpbuf[100];


	string type = info.type;
	
	if (info.type == "set") {
		snprintf (tmpbuf, sizeof(tmpbuf)-1, "/sl/%d/%s", info.instance, info.type.c_str());

		if (lo_send(_addr, tmpbuf, "sf", info.command.c_str(), val) < 0) {
			fprintf(stderr, "OSC error %d: %s\n", lo_address_errno(_addr), lo_address_errstr(_addr));
		}
	}
	else {
		if (type == "note") {
			if (val > 0.0f) {
				cerr << "val is " << val << endl;
				type = "down";
			}
			else {
				type = "up";
			}
		}

		snprintf (tmpbuf, sizeof(tmpbuf)-1, "/sl/%d/%s", info.instance, type.c_str());
		
		if (lo_send(_addr, tmpbuf, "s", info.command.c_str()) < 0) {
			fprintf(stderr, "OSC error %d: %s\n", lo_address_errno(_addr), lo_address_errstr(_addr));
		}
	}
}



void * MidiBridge::_midi_receiver(void *arg)
{
	MidiBridge * bridge = static_cast<MidiBridge*> (arg);

	bridge->midi_receiver();
	return 0;
}

void  MidiBridge::midi_receiver()
{
	struct pollfd pfd[3];
	int nfds = 0;
	int timeout = -1;
	MIDI::byte buf[512];

	while (!_done) {
		nfds = 0;

		pfd[nfds].fd = _midi_request_pipe[0];
		pfd[nfds].events = POLLIN|POLLHUP|POLLERR;
		nfds++;

		if (_port && _port->selectable() >= 0) {
			pfd[nfds].fd = _port->selectable();
			pfd[nfds].events = POLLIN|POLLHUP|POLLERR;
			nfds++;
		}
		
	again:
		// cerr << "poll on " << nfds << " for " << timeout << endl;
		if (poll (pfd, nfds, timeout) < 0) {
			if (errno == EINTR) {
				/* gdb at work, perhaps */
				goto again;
			}
			
			cerr << "MIDI thread poll failed: " <<  strerror (errno) << endl;
			
			break;
		}
		
		if ((pfd[0].revents & ~POLLIN)) {
			cerr << "Transport: error polling extra MIDI port" << endl;
			break;
		}
		
		if (nfds > 1 && pfd[1].revents & POLLIN) {

			/* reading from the MIDI port activates the Parser
			   that in turn generates signals that we care
			   about. the port is already set to NONBLOCK so that
			   can read freely here.
			*/
			
			while (1) {
				
				// cerr << "+++ READ ON " << port->name() << endl;
				
				int nread = _port->read (buf, sizeof (buf));
				
				// cerr << "-- READ (" << nread << " ON " << port->name() << endl;
				
				if (nread > 0) {
					if ((size_t) nread < sizeof (buf)) {
						break;
					} else {
						continue;
					}
				} else if (nread == 0) {
					break;
				} else if (errno == EAGAIN) {
					break;
				} else {
					cerr << "Error reading from midi port" << endl;
					_done = true;
					break;
				}
			}
			
		}
	}

	delete _port;
	_port = 0;
}






