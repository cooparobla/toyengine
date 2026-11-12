/**
 * @file register.h
 * @brief Registers the save components' scene parsers: "SaveId" and the demo "SaveDemo".
 */

#ifndef TOYENGINE_SAVE_REGISTER_H
#define TOYENGINE_SAVE_REGISTER_H

#include <cstdint>
#include <string>



namespace toy {
namespace save {

/** @brief Call once at startup, before the first SceneLoader::load() that uses them. */
void register_save_components();

}  // namespace save
}  // namespace toy

#endif  // TOYENGINE_SAVE_REGISTER_H
