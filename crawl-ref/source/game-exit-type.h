#pragma once

// What caused the game to exit?
// utility functions in end.cc, cf. kill_method_type in ouch.h
enum class game_exit
{
    unknown, // for ordinary games, no previous game this session.
    win,
    leave,
    quit,
    death,
    save,
    // Restart Crawl's game state without ending the process or its WebTiles
    // connection. Housing uses this to restore either the canonical owner
    // checkpoint or a retained visitor rollback package in the same process.
    housing_transition,
    abort, // used when a game is aborted before it starts, e.g.
           // when exiting character selection, or aborting a text
           // entry.
    crash
};
