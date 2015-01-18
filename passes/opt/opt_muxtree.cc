/*
 *  yosys -- Yosys Open SYnthesis Suite
 *
 *  Copyright (C) 2012  Clifford Wolf <clifford@clifford.at>
 *  
 *  Permission to use, copy, modify, and/or distribute this software for any
 *  purpose with or without fee is hereby granted, provided that the above
 *  copyright notice and this permission notice appear in all copies.
 *  
 *  THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 *  WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 *  MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 *  ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 *  WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 *  ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 *  OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 *
 */

#include "kernel/register.h"
#include "kernel/sigtools.h"
#include "kernel/log.h"
#include "kernel/celltypes.h"
#include <stdlib.h>
#include <stdio.h>
#include <set>

USING_YOSYS_NAMESPACE
PRIVATE_NAMESPACE_BEGIN

using RTLIL::id2cstr;

struct OptMuxtreeWorker
{
	RTLIL::Design *design;
	RTLIL::Module *module;
	SigMap assign_map;
	int removed_count;

	struct bitinfo_t {
		int num;
		SigBit bit;
		bool seen_non_mux;
		vector<int> mux_users;
		vector<int> mux_drivers;
	};

	dict<SigBit, int> bit2num;
	vector<bitinfo_t> bit2info;

	struct portinfo_t {
		int ctrl_sig;
		vector<int> input_sigs;
		vector<int> input_muxes;
		bool const_activated;
		bool const_deactivated;
		bool enabled;
	};

	struct muxinfo_t {
		RTLIL::Cell *cell;
		vector<portinfo_t> ports;
	};

	vector<muxinfo_t> mux2info;
	vector<bool> root_muxes;

	OptMuxtreeWorker(RTLIL::Design *design, RTLIL::Module *module) :
			design(design), module(module), assign_map(module), removed_count(0)
	{
		log("Running muxtree optimizier on module %s..\n", module->name.c_str());

		log("  Creating internal representation of mux trees.\n");

		// Populate bit2info[]:
		//	.seen_non_mux
		//	.mux_users
		//	.mux_drivers
		// Populate mux2info[].ports[]:
		//	.ctrl_sig
		//	.input_sigs
		//	.const_activated
		//	.const_deactivated
		for (auto cell : module->cells())
		{
			if (cell->type == "$mux" || cell->type == "$pmux")
			{
				RTLIL::SigSpec sig_a = cell->getPort("\\A");
				RTLIL::SigSpec sig_b = cell->getPort("\\B");
				RTLIL::SigSpec sig_s = cell->getPort("\\S");
				RTLIL::SigSpec sig_y = cell->getPort("\\Y");

				muxinfo_t muxinfo;
				muxinfo.cell = cell;

				for (int i = 0; i < sig_s.size(); i++) {
					RTLIL::SigSpec sig = sig_b.extract(i*sig_a.size(), sig_a.size());
					RTLIL::SigSpec ctrl_sig = assign_map(sig_s.extract(i, 1));
					portinfo_t portinfo;
					portinfo.ctrl_sig = sig2bits(ctrl_sig, false).front();
					for (int idx : sig2bits(sig)) {
						add_to_list(bit2info[idx].mux_users, mux2info.size());
						add_to_list(portinfo.input_sigs, idx);
					}
					portinfo.const_activated = ctrl_sig.is_fully_const() && ctrl_sig.as_bool();
					portinfo.const_deactivated = ctrl_sig.is_fully_const() && !ctrl_sig.as_bool();
					portinfo.enabled = false;
					muxinfo.ports.push_back(portinfo);
				}

				portinfo_t portinfo;
				for (int idx : sig2bits(sig_a)) {
					add_to_list(bit2info[idx].mux_users, mux2info.size());
					add_to_list(portinfo.input_sigs, idx);
				}
				portinfo.ctrl_sig = -1;
				portinfo.const_activated = false;
				portinfo.const_deactivated = false;
				portinfo.enabled = false;
				muxinfo.ports.push_back(portinfo);

				for (int idx : sig2bits(sig_y))
					add_to_list(bit2info[idx].mux_drivers, mux2info.size());

				for (int idx : sig2bits(sig_s))
					bit2info[idx].seen_non_mux = true;

				mux2info.push_back(muxinfo);
			}
			else
			{
				for (auto &it : cell->connections()) {
					for (int idx : sig2bits(it.second))
						bit2info[idx].seen_non_mux = true;
				}
			}
		}
		for (auto wire : module->wires()) {
			if (wire->port_output)
				for (int idx : sig2bits(RTLIL::SigSpec(wire)))
					bit2info[idx].seen_non_mux = true;
		}

		if (mux2info.size() == 0) {
			log("  No muxes found in this module.\n");
			return;
		}

		// Populate mux2info[].ports[]:
		//	.input_muxes
		for (size_t i = 0; i < bit2info.size(); i++)
		for (int j : bit2info[i].mux_users)
		for (auto &p : mux2info[j].ports) {
			if (is_in_list(p.input_sigs, i))
				for (int k : bit2info[i].mux_drivers)
					add_to_list(p.input_muxes, k);
		}

		log("  Evaluating internal representation of mux trees.\n");

		dict<int, pool<int>> mux_to_users;
		root_muxes.resize(mux2info.size());

		for (auto &bi : bit2info) {
			for (int i : bi.mux_drivers)
				for (int j : bi.mux_users)
					mux_to_users[i].insert(j);
			if (!bi.seen_non_mux)
				continue;
			for (int mux_idx : bi.mux_drivers)
				root_muxes.at(mux_idx) = true;
		}

		for (auto &it : mux_to_users)
			if (GetSize(it.second) > 1)
				root_muxes.at(it.first) = true;

		for (int mux_idx = 0; mux_idx < GetSize(root_muxes); mux_idx++)
			if (root_muxes.at(mux_idx)) {
				log("    Root of a mux tree: %s\n", log_id(mux2info[mux_idx].cell));
				eval_root_mux(mux_idx);
			}

		log("  Analyzing evaluation results.\n");

		for (auto &mi : mux2info)
		{
			vector<int> live_ports;
			for (int port_idx = 0; port_idx < GetSize(mi.ports); port_idx++) {
				portinfo_t &pi = mi.ports[port_idx];
				if (pi.enabled) {
					live_ports.push_back(port_idx);
				} else {
					log("    dead port %d/%d on %s %s.\n", port_idx+1, GetSize(mi.ports),
							mi.cell->type.c_str(), mi.cell->name.c_str());
					removed_count++;
				}
			}

			if (live_ports.size() == mi.ports.size())
				continue;

			if (live_ports.size() == 0) {
				module->remove(mi.cell);
				continue;
			}

			RTLIL::SigSpec sig_a = mi.cell->getPort("\\A");
			RTLIL::SigSpec sig_b = mi.cell->getPort("\\B");
			RTLIL::SigSpec sig_s = mi.cell->getPort("\\S");
			RTLIL::SigSpec sig_y = mi.cell->getPort("\\Y");

			RTLIL::SigSpec sig_ports = sig_b;
			sig_ports.append(sig_a);

			if (live_ports.size() == 1)
			{
				RTLIL::SigSpec sig_in = sig_ports.extract(live_ports[0]*sig_a.size(), sig_a.size());
				module->connect(RTLIL::SigSig(sig_y, sig_in));
				module->remove(mi.cell);
			}
			else
			{
				RTLIL::SigSpec new_sig_a, new_sig_b, new_sig_s;

				for (size_t i = 0; i < live_ports.size(); i++) {
					RTLIL::SigSpec sig_in = sig_ports.extract(live_ports[i]*sig_a.size(), sig_a.size());
					if (i == live_ports.size()-1) {
						new_sig_a = sig_in;
					} else {
						new_sig_b.append(sig_in);
						new_sig_s.append(sig_s.extract(live_ports[i], 1));
					}
				}

				mi.cell->setPort("\\A", new_sig_a);
				mi.cell->setPort("\\B", new_sig_b);
				mi.cell->setPort("\\S", new_sig_s);
				if (new_sig_s.size() == 1) {
					mi.cell->type = "$mux";
					mi.cell->parameters.erase("\\S_WIDTH");
				} else {
					mi.cell->parameters["\\S_WIDTH"] = RTLIL::Const(new_sig_s.size());
				}
			}
		}
	}

	bool is_in_list(const vector<int> &list, int value)
	{
		for (int v : list)
			if (v == value)
				return true;
		return false;
	}

	void add_to_list(vector<int> &list, int value)
	{
		if (!is_in_list(list, value))
			list.push_back(value);
	}

	vector<int> sig2bits(RTLIL::SigSpec sig, bool skip_non_wires = true)
	{
		vector<int> results;
		assign_map.apply(sig);
		for (auto &bit : sig)
			if (bit.wire != NULL) {
				if (bit2num.count(bit) == 0) {
					bitinfo_t info;
					info.num = bit2info.size();
					info.bit = bit;
					info.seen_non_mux = false;
					bit2info.push_back(info);
					bit2num[info.bit] = info.num;
				}
				results.push_back(bit2num[bit]);
			} else if (!skip_non_wires)
				results.push_back(-1);
		return results;
	}

	struct knowledge_t
	{
		// database of known inactive signals
		// the payload is a reference counter used to manage the
		// list. when it is non-zero the signal in known to be inactive
		vector<int> known_inactive;

		// database of known active signals
		vector<int> known_active;

		// this is just used to keep track of visited muxes in order to prohibit
		// endless recursion in mux loops
		vector<bool> visited_muxes;
	};

	void eval_mux_port(knowledge_t &knowledge, int mux_idx, int port_idx)
	{
		muxinfo_t &muxinfo = mux2info[mux_idx];

		if (muxinfo.ports[port_idx].const_deactivated)
			return;

		muxinfo.ports[port_idx].enabled = true;

		for (int i = 0; i < GetSize(muxinfo.ports); i++) {
			if (i == port_idx)
				continue;
			if (muxinfo.ports[i].ctrl_sig >= 0)
				knowledge.known_inactive.at(muxinfo.ports[i].ctrl_sig)++;
		}

		if (port_idx < int(muxinfo.ports.size())-1 && !muxinfo.ports[port_idx].const_activated)
			knowledge.known_active.at(muxinfo.ports[port_idx].ctrl_sig)++;

		vector<int> parent_muxes;
		for (int m : muxinfo.ports[port_idx].input_muxes) {
			if (knowledge.visited_muxes[m])
				continue;
			knowledge.visited_muxes[m] = true;
			parent_muxes.push_back(m);
		}
		for (int m : parent_muxes)
			if (!root_muxes.at(m))
				eval_mux(knowledge, m);
		for (int m : parent_muxes)
			knowledge.visited_muxes[m] = false;

		if (port_idx < int(muxinfo.ports.size())-1 && !muxinfo.ports[port_idx].const_activated)
			knowledge.known_active.at(muxinfo.ports[port_idx].ctrl_sig)--;

		for (size_t i = 0; i < muxinfo.ports.size(); i++) {
			if (int(i) == port_idx)
				continue;
			if (muxinfo.ports[i].ctrl_sig >= 0)
				knowledge.known_inactive.at(muxinfo.ports[i].ctrl_sig)--;
		}
	}

	void replace_known(knowledge_t &knowledge, muxinfo_t &muxinfo, IdString portname)
	{
		SigSpec sig = muxinfo.cell->getPort(portname);
		bool did_something = false;

		vector<int> bits = sig2bits(sig, false);
		for (int i = 0; i < GetSize(bits); i++) {
			if (bits[i] < 0)
				continue;
			if (knowledge.known_inactive.at(bits[i])) {
				sig[i] = State::S0;
				did_something = true;
			} else
			if (knowledge.known_active.at(bits[i])) {
				sig[i] = State::S1;
				did_something = true;
			}
		}

		if (did_something) {
			log("      Replacing known input bits on port %s of cell %s: %s -> %s\n", log_id(portname),
					log_id(muxinfo.cell), log_signal(muxinfo.cell->getPort(portname)), log_signal(sig));
			muxinfo.cell->setPort(portname, sig);
		}
	}

	void eval_mux(knowledge_t &knowledge, int mux_idx)
	{
		muxinfo_t &muxinfo = mux2info[mux_idx];

		// set input ports to constants if we find known active or inactive signals
		replace_known(knowledge, muxinfo, "\\A");
		replace_known(knowledge, muxinfo, "\\B");

		// if there is a constant activated port we just use it
		for (size_t port_idx = 0; port_idx < muxinfo.ports.size()-1; port_idx++)
		{
			portinfo_t &portinfo = muxinfo.ports[port_idx];
			if (portinfo.const_activated) {
				eval_mux_port(knowledge, mux_idx, port_idx);
				return;
			}
		}

		// compare ports with known_active signals. if we find a match, only this
		// port can be active. do not include the last port (its the default port
		// that has no control signals).
		for (size_t port_idx = 0; port_idx < muxinfo.ports.size()-1; port_idx++)
		{
			portinfo_t &portinfo = muxinfo.ports[port_idx];
			if (knowledge.known_active.at(portinfo.ctrl_sig)) {
				eval_mux_port(knowledge, mux_idx, port_idx);
				return;
			}
		}

		// compare ports with known_inactive and known_active signals. If the control
		// signal of the port is known_inactive or if the control signals of all other
		// ports are known_active this port can't be activated. this loop includes the
		// default port but no known_inactive match is performed on the default port.
		for (size_t port_idx = 0; port_idx < muxinfo.ports.size(); port_idx++)
		{
			portinfo_t &portinfo = muxinfo.ports[port_idx];

			if (port_idx < muxinfo.ports.size()-1) {
				bool found_non_known_inactive = false;
				if (knowledge.known_inactive.at(portinfo.ctrl_sig) == 0)
					found_non_known_inactive = true;
				if (!found_non_known_inactive)
					continue;
			}

			bool port_active = true;
			for (size_t i = 0; i < muxinfo.ports.size()-1; i++) {
				if (i == port_idx)
					continue;
				if (knowledge.known_active.at(muxinfo.ports[i].ctrl_sig))
					port_active = false;
			}
			if (port_active)
				eval_mux_port(knowledge, mux_idx, port_idx);
		}
	}

	void eval_root_mux(int mux_idx)
	{
		knowledge_t knowledge;
		knowledge.known_inactive.resize(bit2info.size());
		knowledge.known_active.resize(bit2info.size());
		knowledge.visited_muxes.resize(mux2info.size());
		knowledge.visited_muxes[mux_idx] = true;
		eval_mux(knowledge, mux_idx);
	}
};

struct OptMuxtreePass : public Pass {
	OptMuxtreePass() : Pass("opt_muxtree", "eliminate dead trees in multiplexer trees") { }
	virtual void help()
	{
		//   |---v---|---v---|---v---|---v---|---v---|---v---|---v---|---v---|---v---|---v---|
		log("\n");
		log("    opt_muxtree [selection]\n");
		log("\n");
		log("This pass analyzes the control signals for the multiplexer trees in the design\n");
		log("and identifies inputs that can never be active. It then removes this dead\n");
		log("branches from the multiplexer trees.\n");
		log("\n");
		log("This pass only operates on completely selected modules without processes.\n");
		log("\n");
	}
	virtual void execute(vector<std::string> args, RTLIL::Design *design)
	{
		log_header("Executing OPT_MUXTREE pass (detect dead branches in mux trees).\n");
		extra_args(args, 1, design);

		int total_count = 0;
		for (auto mod : design->modules()) {
			if (!design->selected_whole_module(mod)) {
				if (design->selected(mod))
					log("Skipping module %s as it is only partially selected.\n", log_id(mod));
				continue;
			}
			if (mod->processes.size() > 0) {
				log("Skipping module %s as it contains processes.\n", log_id(mod));
			} else {
				OptMuxtreeWorker worker(design, mod);
				total_count += worker.removed_count;
			}
		}
		if (total_count)
			design->scratchpad_set_bool("opt.did_something", true);
		log("Removed %d multiplexer ports.\n", total_count);
	}
} OptMuxtreePass;
 
PRIVATE_NAMESPACE_END
