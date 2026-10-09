// root_directory.h.in
#pragma once

// The toyengine checkout this binary was built from: the repo itself, or a project's
// .libs/toyengine. Engine-owned files (shaders, fonts, editor themes, the fallback assets/)
// resolve against it.
#define ROOT_DIR "/Users/tblaney/Code/toyengine"
#define PROJ_DIR "/Users/tblaney/Code/toyengine/libs"

// The project directory whose assets/ the game and editor load. toyengine_add_project()
// (cmake/ToyProject.cmake) defines it per executable; anything else (the engine's own tests)
// is building toyengine itself, whose project is the checkout.
#ifndef TOY_PROJECT_ROOT
#define TOY_PROJECT_ROOT ROOT_DIR
#endif
