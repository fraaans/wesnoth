/*
   Copyright (C) 2003-2005 by David White <dave@whitevine.net>
   Copyright (C) 2005 - 2014 by Philippe Plantier <ayin@anathas.org>
   Part of the Battle for Wesnoth Project http://www.wesnoth.org

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 2 of the License, or
   (at your option) any later version.
   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY.

   See the COPYING file for more details.
*/

/**
 *  @file
 *  Controls setup, play, (auto)save and replay of campaigns.
 */

#include "global.hpp"

#include "playcampaign.hpp"

#include "carryover.hpp"
#include "game_preferences.hpp"
#include "generators/map_create.hpp"
#include "gui/dialogs/message.hpp"
#include "gui/dialogs/transient_message.hpp"
#include "gui/widgets/window.hpp"
#include "persist_manager.hpp"
#include "playmp_controller.hpp"
#include "replay_controller.hpp"
#include "log.hpp"
#include "map_exception.hpp"
#include "mp_game_utils.hpp"
#include "multiplayer.hpp"
#include "multiplayer_connect_engine.hpp"
#include "dialogs.hpp"
#include "gettext.hpp"
#include "resources.hpp"
#include "savegame.hpp"
#include "saved_game.hpp"
#include "sound.hpp"
#include "wml_exception.hpp"
#include "unit_id.hpp"
#include "formula_string_utils.hpp"

#include <boost/foreach.hpp>

#define LOG_G LOG_STREAM(info, lg::general)

static lg::log_domain log_engine("engine");
#define LOG_NG LOG_STREAM(info, log_engine)
#define ERR_NG LOG_STREAM(err, log_engine)

static lg::log_domain log_enginerefac("enginerefac");
#define LOG_RG LOG_STREAM(info, log_enginerefac)

static void store_carryover(saved_game& gamestate, playsingle_controller& playcontroller, display& disp, const end_level_data& end_level, const LEVEL_RESULT res){
	bool has_next_scenario = !resources::gamedata->next_scenario().empty() &&
			resources::gamedata->next_scenario() != "null";
	//explain me: when could this be the case??
	if(resources::teams->size() < 1){
		gamestate.carryover_sides_start["next_scenario"] = resources::gamedata->next_scenario();
		return;
	}

	std::ostringstream report;
	std::string title;

	bool obs = is_observer();

	if (obs) {
		title = _("Scenario Report");
	} else if (res == VICTORY) {
		title = _("Victory");
		report << "<b>" << _("You have emerged victorious!") << "</b>\n\n";
	} else {
		title = _("Defeat");
		report <<  _("You have been defeated!") << "\n";
	}

	std::vector<team> teams = playcontroller.get_teams_const();
	int persistent_teams = 0;
	BOOST_FOREACH(const team &t, teams) {
		if (t.persistent()){
			++persistent_teams;
		}
	}

	if (persistent_teams > 0 && ((has_next_scenario && end_level.proceed_to_next_level)||
			gamestate.classification().campaign_type == game_classification::TEST))
	{
		gamemap map = playcontroller.get_map_const();
		int finishing_bonus_per_turn =
				map.villages().size() * game_config::village_income +
				game_config::base_income;
		tod_manager tod = playcontroller.get_tod_manager_const();
		int turns_left = std::max<int>(0, tod.number_of_turns() - tod.turn());
		int finishing_bonus = (end_level.gold_bonus && turns_left > -1) ?
				finishing_bonus_per_turn * turns_left : 0;


		BOOST_FOREACH(const team &t, teams)
		{
			if (!t.persistent() || t.lost() || !t.is_human())
			{
				continue;
			}
			int carryover_gold = div100rounded((t.gold() + finishing_bonus) * end_level.carryover_percentage);
			
			if (persistent_teams > 1) {
				report << "\n<b>" << t.current_player() << "</b>\n";
			}

			playcontroller.report_victory(report, carryover_gold, t.gold(), finishing_bonus_per_turn, turns_left, finishing_bonus);
		}
	}

	if (end_level.transient.carryover_report) {
		gui2::show_transient_message(disp.video(), title, report.str(), "", true);
	}
}

static void generate_scenario(config const*& scenario)
{
	LOG_G << "randomly generating scenario...\n";
	const cursor::setter cursor_setter(cursor::WAIT);

	static config new_scenario;
	new_scenario = random_generate_scenario((*scenario)["scenario_generation"],
		scenario->child("generator"));

	//TODO comment or remove
	//level_ = scenario;
	//merge carryover information into the newly generated scenario

	scenario = &new_scenario;
}

static void generate_map(config const*& scenario)
{
	LOG_G << "randomly generating map...\n";
	const cursor::setter cursor_setter(cursor::WAIT);

	const std::string map_data = random_generate_map(
		(*scenario)["map_generation"], scenario->child("generator"));

	// Since we've had to generate the map,
	// make sure that when we save the game,
	// it will not ask for the map to be generated again on reload
	static config new_scenario;
	new_scenario = *scenario;
	new_scenario["map_data"] = map_data;
	scenario = &new_scenario;
}

LEVEL_RESULT play_replay(display& disp, saved_game& gamestate, const config& game_config,
		CVideo& video, bool is_unit_test)
{
	const std::string campaign_type_str = lexical_cast<std::string> (gamestate.classification().campaign_type);

	// 'starting_pos' will contain the position we start the game from.
	const config& starting_pos = gamestate.replay_start();

	//for replays, use the variables specified in starting_pos
	if (const config &vars = starting_pos.child("variables")) {
		gamestate.carryover_sides_start.child_or_add("variables") = vars;
	}

	try {
		// Preserve old label eg. replay
		if (gamestate.classification().label.empty())
			gamestate.classification().label = starting_pos["name"].str();
		//if (gamestate.abbrev.empty())
		//	gamestate.abbrev = (*scenario)["abbrev"];

		LEVEL_RESULT res = play_replay_level(game_config, video, gamestate, is_unit_test);

		recorder.clear();
		gamestate.replay_data.clear();

		return res;
	} catch(game::load_game_failed& e) {
		if (is_unit_test) {
			std::cerr << std::string(_("The game could not be loaded: ")) + " (game::load_game_failed) " + e.message << std::endl;
			return DEFEAT;
		} else {
			gui2::show_error_message(disp.video(), _("The game could not be loaded: ") + e.message);
		}
	} catch(game::game_error& e) {
		if (is_unit_test) {
			std::cerr << std::string(_("Error while playing the game: ")) + " (game::game_error) " + e.message << std::endl;
			return DEFEAT;
		} else {
			gui2::show_error_message(disp.video(), std::string(_("Error while playing the game: ")) + e.message);
		}
	} catch(incorrect_map_format_error& e) {
		if (is_unit_test) {
			std::cerr << std::string(_("The game map could not be loaded: ")) + " (incorrect_map_format_error) " + e.message << std::endl;
			return DEFEAT;
		} else {
			gui2::show_error_message(disp.video(), std::string(_("The game map could not be loaded: ")) + e.message);
		}
	} catch(twml_exception& e) {
		if (is_unit_test) {
			std::cerr << std::string("WML Exception: ") + e.user_message << std::endl;
			std::cerr << std::string("Dev Message: ") + e.dev_message << std::endl;
			return DEFEAT;
		} else {
			e.show(disp);
		}
	}
	return NONE;
}

static LEVEL_RESULT playsingle_scenario(const config& game_config,
		display& disp, saved_game& state_of_game,
		const config::const_child_itors &story,
		bool skip_replay, end_level_data &end_level)
{
	const int ticks = SDL_GetTicks();
	
	state_of_game.expand_carryover();
	
	LOG_NG << "creating objects... " << (SDL_GetTicks() - ticks) << "\n";
	playsingle_controller playcontroller(state_of_game.get_starting_pos(), state_of_game, ticks, game_config, disp.video(), skip_replay);
	LOG_NG << "created objects... " << (SDL_GetTicks() - playcontroller.get_ticks()) << "\n";

	LEVEL_RESULT res = playcontroller.play_scenario(story, skip_replay);

	end_level = playcontroller.get_end_level_data_const();
	config& cfg_end_level = state_of_game.carryover_sides.child_or_add("end_level_data");
	state_of_game.carryover_sides["next_underlying_unit_id"] = int(n_unit::id_manager::instance().get_save_id());
	end_level.write(cfg_end_level);

	if (res != QUIT)
	{
		//if we are loading from linger mode then we already did this.
		if(res != SKIP_TO_LINGER)
		{
			store_carryover(state_of_game, playcontroller, disp, end_level, res);
		}
		if(!disp.video().faked())
		{
			try {
				playcontroller.maybe_linger();
			} catch(end_level_exception& e) {
				if (e.result == QUIT) {
					return QUIT;
				}
			}
		}
	}
	state_of_game.set_snapshot(playcontroller.to_config());

	return res;
}


static LEVEL_RESULT playmp_scenario(const config& game_config,
		display& disp, saved_game& state_of_game,
		const config::const_child_itors &story, bool skip_replay,
		bool blindfold_replay, io_type_t& io_type, end_level_data &end_level)
{
	const int ticks = SDL_GetTicks();
	state_of_game.expand_carryover();

	playmp_controller playcontroller(state_of_game.get_starting_pos(), state_of_game, ticks,
		game_config, disp.video(), skip_replay, blindfold_replay, io_type == IO_SERVER);
	LEVEL_RESULT res = playcontroller.play_scenario(story, skip_replay);

	end_level = playcontroller.get_end_level_data_const();
	config& cfg_end_level = state_of_game.carryover_sides.child_or_add("end_level_data");
	end_level.write(cfg_end_level);
	state_of_game.carryover_sides["next_underlying_unit_id"] = int(n_unit::id_manager::instance().get_save_id());

	//Check if the player started as mp client and changed to host
	if (io_type == IO_CLIENT && playcontroller.is_host())
		io_type = IO_SERVER;

	if (res != QUIT)
	{
		if(res != OBSERVER_END && res != SKIP_TO_LINGER)
		{
			//We need to call this before linger because it also prints the defeated/victory message.
			//(we want to see that message before entering the linger mode)
			store_carryover(state_of_game, playcontroller, disp, end_level, res);
		}
		else
		{
			state_of_game.carryover_sides_start["next_scenario"] = resources::gamedata->next_scenario();
		}
		if(!disp.video().faked())
		{
			try {
				playcontroller.maybe_linger();
			} catch(end_level_exception& e) {
				if (e.result == QUIT) {
					return QUIT;
				}
			}
		}

	}
	state_of_game.set_snapshot(playcontroller.to_config());
	return res;
}

LEVEL_RESULT play_game(game_display& disp, saved_game& gamestate,
	const config& game_config, io_type_t io_type, bool skip_replay,
	bool network_game, bool blindfold_replay, bool is_unit_test)
{
	const std::string campaign_type_str = lexical_cast_default<std::string> (gamestate.classification().campaign_type);

	gamestate.expand_scenario();

	while(gamestate.valid()) {
		config& starting_pos = gamestate.get_starting_pos();
		config::const_child_itors story = starting_pos.child_range("story");


		bool save_game_after_scenario = true;

		LEVEL_RESULT res = VICTORY;
		end_level_data end_level;

		try {
			// Preserve old label eg. replay
			if (gamestate.classification().label.empty()) {
				if (gamestate.classification().abbrev.empty())
					gamestate.classification().label = starting_pos["name"].str();
				else {
					gamestate.classification().label = gamestate.classification().abbrev;
					gamestate.classification().label.append("-");
					gamestate.classification().label.append(starting_pos["name"]);
				}
			}

			// If the entire scenario should be randomly generated
			if(starting_pos["scenario_generation"] != "") {
				const config * tmp = &starting_pos;
				generate_scenario(tmp);
			}
			std::string map_data = starting_pos["map_data"];
			if(map_data.empty() && starting_pos["map"] != "") {
				map_data = read_map(starting_pos["map"]);
			}

			// If the map should be randomly generated
			if(map_data.empty() && starting_pos["map_generation"] != "") {
				const config * tmp = &starting_pos;
				generate_map(tmp);
			}

			sound::empty_playlist();

			switch (io_type){
			case IO_NONE:
#if !defined(ALWAYS_USE_MP_CONTROLLER)
				res = playsingle_scenario(game_config, disp, gamestate, story, skip_replay, end_level);
				break;
#endif
			case IO_SERVER:
			case IO_CLIENT:
				res = playmp_scenario(game_config, disp, gamestate, story, skip_replay, blindfold_replay, io_type, end_level);
				break;
			}
		} catch(game::load_game_failed& e) {
			gui2::show_error_message(disp.video(), _("The game could not be loaded: ") + e.message);
			return QUIT;
		} catch(game::game_error& e) {
			gui2::show_error_message(disp.video(), _("Error while playing the game: ") + e.message);
			return QUIT;
		} catch(incorrect_map_format_error& e) {
			gui2::show_error_message(disp.video(), std::string(_("The game map could not be loaded: ")) + e.message);
			return QUIT;
		} catch(config::error& e) {
			std::cerr << "caught config::error...\n";
			gui2::show_error_message(disp.video(), _("Error while reading the WML: ") + e.message);
			return QUIT;
		} catch(twml_exception& e) {
			e.show(disp);
			return QUIT;
		}

		if (is_unit_test) {
			return res;
		}

		// Save-management options fire on game end.
		// This means: (a) we have a victory, or
		// or (b) we're multiplayer live, in which
		// case defeat is also game end.  Someday,
		// if MP campaigns ever work again, we might
		// need to change this test.
		if (res == VICTORY || (io_type != IO_NONE && res == DEFEAT)) {
			if (preferences::delete_saves())
				savegame::clean_saves(gamestate.classification().label);

			if (preferences::save_replays() && end_level.replay_save) {
				savegame::replay_savegame save(gamestate, preferences::save_compression_format());
				save.save_game_automatic(disp.video(), true);
			}
		}
		
		gamestate.convert_to_start_save();
		recorder.clear();

		// On DEFEAT, QUIT, or OBSERVER_END, we're done now

		//If there is no next scenario we're done now.
		if(res == QUIT || !end_level.proceed_to_next_level || gamestate.carryover_sides_start["next_scenario"].empty())
		{
			return res;
		}
		else if(res == OBSERVER_END)
		{
			const int dlg_res = gui2::show_message(disp.video(), _("Game Over"),
				_("This scenario has ended. Do you want to continue the campaign?"),
				gui2::tmessage::yes_no_buttons);

			if(dlg_res == gui2::twindow::CANCEL) {
				return res;
			}
		}

		// Continue without saving is like a victory,
		// but the save game dialog isn't displayed
		if (!end_level.prescenario_save)
			save_game_after_scenario = false;

		if (io_type == IO_CLIENT) {
			// Opens mp::connect dialog to get a new gamestate.
			mp::ui::result wait_res = mp::goto_mp_wait(gamestate, disp,
				game_config, res == OBSERVER_END);
			if (wait_res == mp::ui::QUIT) {
				return QUIT;
			}

			gamestate.set_scenario(gamestate.replay_start());
			gamestate.replay_start() = config();
			// Retain carryover_sides_start, as the config from the server
			// doesn't contain it.
			//TODO: enable this again or make mp_wait not change carryover sides start.
			//gamestate.carryover_sides_start = sides.to_config();
		} else {
			// Retrieve next scenario data.
			gamestate.expand_scenario();

			if (io_type == IO_SERVER && gamestate.valid()) {
				config * scenario = &gamestate.get_starting_pos();
				mp_game_settings& params = gamestate.mp_settings();

				// A hash have to be generated using an unmodified
				// scenario data.
				params.hash = scenario->hash();

				// Apply carryover before passing a scenario data to the
				// mp::connect_engine.
				gamestate.expand_carryover();
				
				//We don't merge WML until start of next scenario, but if we want to allow user to disable MP ui in transition,
				//then we have to move "allow_new_game" attribute over now.
				bool allow_new_game_flag = (*scenario)["allow_new_game"].to_bool(true);

				if (gamestate.carryover_sides_start.child_or_empty("end_level_data").child_or_empty("next_scenario_settings").has_attribute("allow_new_game")) {
					allow_new_game_flag = gamestate.carryover_sides_start.child_or_empty("end_level_data").child("next_scenario_settings")["allow_new_game"].to_bool();
				}

				params.scenario_data = *scenario;
				params.scenario_data["next_underlying_unit_id"] = n_unit::id_manager::instance().get_save_id();
				params.mp_scenario = (*scenario)["id"].str();
				params.mp_scenario_name = (*scenario)["name"].str();
				params.num_turns = (*scenario)["turns"].to_int(-1);
				params.saved_game = false;
				params.use_map_settings =
					(*scenario)["force_lock_settings"].to_bool();

				mp::connect_engine_ptr
					connect_engine(new mp::connect_engine(disp, gamestate,
						params, !network_game, false));

				if (allow_new_game_flag || (game_config::debug && network::nconnections() == 0)) {
					// Opens mp::connect dialog to allow users to
					// make an adjustments for scenario.
					// TODO: Fix this so that it works when network::nconnections() > 0 as well.
					mp::ui::result connect_res = mp::goto_mp_connect(disp,
						*connect_engine, game_config, params.name);
					if (connect_res == mp::ui::QUIT) {
						return QUIT;
					}
				} else {
					// Start the next scenario immediately.
					connect_engine->
						start_game(mp::connect_engine::FORCE_IMPORT_USERS);
				}

				starting_pos = gamestate.replay_start();
				scenario = &starting_pos;

				// TODO: move this code to mp::connect_engine
				// in order to send generated data to the network
				// before starting the game.
				//
				// If the entire scenario should be randomly generated
				/*if((*scenario)["scenario_generation"] != "") {
					generate_scenario(scenario);
				}

				std::string map_data = (*scenario)["map_data"];
				if(map_data.empty() && (*scenario)["map"] != "") {
					map_data = read_map((*scenario)["map"]);
				}

				// If the map should be randomly generated
				if(map_data.empty() && (*scenario)["map_generation"] != "") {
					generate_map(scenario);
				}*/
			}
		}

		if(gamestate.valid()) {
			// Update the label
			if (gamestate.classification().abbrev.empty())
				gamestate.classification().label = starting_pos["name"].str();
			else {
				gamestate.classification().label = gamestate.classification().abbrev;
				gamestate.classification().label.append("-");
				gamestate.classification().label.append(starting_pos["name"]);
			}

			// If this isn't the last scenario, then save the game
			if(save_game_after_scenario) {

				// For multiplayer, we want the save
				// to contain the starting position.
				// For campaigns however, this is the
				// start-of-scenario save and the
				// starting position needs to be empty,
				// to force a reload of the scenario config.

				savegame::scenariostart_savegame save(gamestate, preferences::save_compression_format());

				save.save_game_automatic(disp.video());
			}

		}
	}

	if (!gamestate.carryover_sides_start["next_scenario"].empty() && gamestate.carryover_sides_start["next_scenario"] != "null") {
		std::string message = _("Unknown scenario: '$scenario|'");
		utils::string_map symbols;
		symbols["scenario"] = gamestate.carryover_sides_start["next_scenario"];
		message = utils::interpolate_variables_into_string(message, &symbols);
		gui2::show_error_message(disp.video(), message);
		return QUIT;
	}

	if (gamestate.classification().campaign_type == game_classification::SCENARIO){
		if (preferences::delete_saves()) {
			savegame::clean_saves(gamestate.classification().label);
		}
	}
	return VICTORY;
}

