// AdaptivePAProcessor.cpp
// OrcaSlicer
//
// Implementation of the AdaptivePAProcessor class, responsible for processing G-code layers with adaptive pressure advance.

#include "../GCode.hpp"
#include "libslic3r/GCode/AdaptivePAInterpolator.hpp"
#include "libslic3r/libslic3r.h"
#include "AdaptivePAProcessor.hpp"
#include <memory>
#include <cstddef>
#include <regex>
#include <iosfwd>
#include <algorithm>
#include <exception>
#include <sstream>
#include <iostream>
#include <cmath>
#include <cstdlib>
#include <string_view>
#include <cctype>
#include <string>
#include <utility>
#include "libslic3r/Config.hpp"
#include "libslic3r/GCodeWriter.hpp"
#include "libslic3r/PrintConfig.hpp"

namespace Slic3r {

/**
 * @brief Constructor for AdaptivePAProcessor.
 *
 * This constructor initializes the AdaptivePAProcessor with a reference to a GCode object.
 * It also initializes the configuration reference, pressure advance interpolation object,
 * and regular expression patterns used for processing the G-code.
 *
 * @param gcodegen A reference to the GCode object that generates the G-code.
 */
AdaptivePAProcessor::AdaptivePAProcessor(GCode &gcodegen)
    : m_gcodegen(gcodegen),
      m_config(gcodegen.config()),
      m_last_predicted_pa(0.0),
      m_max_next_feedrate(0.0),
      m_next_feedrate(0.0),
      m_current_feedrate(0.0),
      m_last_config_index(-1),
      m_pa_change_pattern(R"(; PA_CHANGE:T(\d+) MM3MM:([0-9]*\.[0-9]+) ACCEL:(\d+) BR:(\d+) RC:(\d+) OV:(\d+))"),
      m_g1_f_pattern(R"(G1 F([0-9]+))")
{
    const size_t indices = std::max(m_config.adaptive_pressure_advance.size(), m_config.enable_pressure_advance.size());
    for (size_t i = 0; i < indices && !m_enabled; ++i)
        m_enabled = m_config.adaptive_pressure_advance.get_at(i) && m_config.enable_pressure_advance.get_at(i);
}

// Method to get the interpolator for a specific filament config index.
// The model is built the first time an index is requested, as the indices in use depend on
// the extruder variant each filament prints with.
AdaptivePAInterpolator* AdaptivePAProcessor::getInterpolator(unsigned int config_index) {
    auto [it, inserted] = m_AdaptivePAInterpolators.try_emplace(config_index);
    // Only enable model for the index if both PA and adaptive PA options are enabled
    if (inserted && m_config.adaptive_pressure_advance.get_at(config_index) && m_config.enable_pressure_advance.get_at(config_index)) {
        it->second = std::make_unique<AdaptivePAInterpolator>();
        it->second->parseAndSetData(m_config.adaptive_pressure_advance_model.get_at(config_index));
    }
    return it->second.get();
}

/**
 * @brief Processes a layer of G-code and applies adaptive pressure advance.
 *
 * This method processes the G-code for a single layer, identifying the appropriate
 * pressure advance settings and applying them based on the current state and configurations.
 *
 * @param gcode A string containing the G-code for the layer.
 * @return A string containing the processed G-code with adaptive pressure advance applied.
 */
std::string AdaptivePAProcessor::process_layer(std::string &&gcode) {
    // Without PA_CHANGE tags the loop below would only terminate the layer's last line.
    if (!m_enabled && gcode.find("; PA_CHANGE") == std::string::npos) {
        if (!gcode.empty() && gcode.back() != '\n')
            gcode += '\n';
        return std::move(gcode);
    }
    // The layer is walked line by line as views into `gcode`, which stays null terminated, so numbers are parsed
    // in place. The look-ahead after a PA_CHANGE tag rescans only the lines of that feature.
    const std::string_view layer(gcode);
    size_t next_line_start = 0;
    // Returns the next line without its trailing '\n'; false at the end of the layer.
    auto get_line = [&layer](size_t &pos, std::string_view &line) {
        if (pos >= layer.size())
            return false;
        const size_t end = std::min(layer.find('\n', pos), layer.size());
        line = layer.substr(pos, end - pos);
        pos  = end + 1;
        return true;
    };
    auto starts_with = [](std::string_view line, std::string_view prefix) { return line.substr(0, prefix.size()) == prefix; };
    auto contains    = [](std::string_view line, std::string_view what) { return line.find(what) != std::string_view::npos; };
    // Feedrate of a "G1 F" line, in mm/s.
    auto feedrate_of = [](std::string_view line) { return std::strtod(line.data() + line.find('F') + 1, nullptr) / 60.0; };

    std::string output;
    output.reserve(gcode.size() + gcode.size() / 64);
    double mm3mm_value = 0.0;
    unsigned int accel_value = 0;
    std::string_view pa_change_line;
    bool wipe_command = false;
    std::string_view line;

    // Iterate through each line of the layer G-code
    while (get_line(next_line_start, line)) {
        
        // If a wipe start command is found, ignore all speed changes till the wipe end part is found
        if (contains(line, "WIPE_START")) {
            wipe_command = true;
        }
                
        // Update current feed rate (this is preceding an extrude or wipe command only). Ignore any speed changes that are emitted during a wipe move.
        // Travel feedrate is output as part of a G1 X Y (Z) F command
        if (starts_with(line, "G1 F") && !wipe_command) { // prune lines quickly before running pattern matching
            m_current_feedrate = feedrate_of(line);
        }
        
        // Wipe end found, continue searching for current feed rate.
        if (contains(line, "WIPE_END")) {
            wipe_command = false;
        }
        
        // Reset next feedrate to zero enable searching for the first encountered
        // feedrate change command after the PA change tag.
        m_next_feedrate = 0;
        
        // Check for PA_CHANGE pattern in the line
        // We will only find this pattern for extruders where adaptive PA is enabled.
        // If there is mixed extruders in the layer (i.e. with adaptive PA on and off
        // this will only update the extruders where the adaptive PA is enabled
        // as these are the only ones where the PA pattern is output
        // For a mixed extruder layer with both adaptive PA enabled and disabled when the new tool is selected
        // the PA for that material is set. As no tag below will be found for this extruder, the original PA is retained.
        std::cmatch match;
        if (starts_with(line, "; PA_CHANGE")) { // prune lines quickly before running regex check as regex is more expensive to run
            if (std::regex_search(line.data(), line.data() + line.size(), match, m_pa_change_pattern)) {
                // The tag carries the filament config index, which selects the PA settings of the filament's extruder variant
                int config_index = std::stoi(match[1].str());
                mm3mm_value = std::stod(match[2].str());
                accel_value = std::stod(match[3].str());
                int isBridge = std::stoi(match[4].str());
                int roleChange = std::stoi(match[5].str());
                int isOverhang = std::stoi(match[6].str());
                
                // Check if the filament config index has changed
                bool config_index_changed = (config_index != m_last_config_index);
                m_last_config_index = config_index;
                
                // Save the PA_CHANGE line to output later after finding feedrate
                pa_change_line = line;
                
                // Look ahead for feedrate before any line containing both G and E commands
                size_t look_ahead = next_line_start;
                std::string_view next_line;
                double temp_feed_rate = 0;
                bool extrude_move_found = false;
                int line_counter = 0;
                
                // Carry on searching on the layer gcode lines to find the print speed
                // If a G1 Fxxxx pattern is found, the new speed is identified
                // Carry on searching for feedrates to find the maximum print speed
                // until a feature change pattern or a wipe command is detected
                while (get_line(look_ahead, next_line)) {
                    line_counter++;
                    // Found an extrude move, set extrude move found flag and move to the next line
                    const bool is_g1 = starts_with(next_line, "G1 ");
                    if ((!extrude_move_found) && is_g1 && contains(next_line, "X") && contains(next_line, "Y") && contains(next_line, "E")) {
                        // Pattern matched, break the loop
                        extrude_move_found = true;
                        continue;
                    }
                    
                    // Found a travel move after we've found at least one extrude move
                    // We now need to stop searching for speeds as we're done printing this island
                    if (is_g1 &&
                        contains(next_line, "X") &&  // X is present
                        contains(next_line, "Y") &&  // Y is present
                        !contains(next_line, "E") && // no "E" present
                        extrude_move_found) {        // An extrude move has happened already
                        // First travel move after extrude move found. Stop searching
                        break;
                    }
                    
                    // Found a WIPE command
                    // If we have a wipe command, usually the wipe speed is different (larger) than the max print speed
                    // for that feature. So stop searching if a wipe command is found because we do not want to overwrite the
                    // speed used for PA calculation by the Wipe speed.
                    if (contains(next_line, "WIPE")) {
                        break; // Stop searching if wipe command is found
                    }
                    
                    // Found another PA_CHANGE pattern
                    // If RC = 1, it means we have a role change, so stop trying to find the max speed for the feature.
                    // This is possibly redundant as a new feature would always have a travel move preceding it
                    // but check anyway. However check last so to not invoke it without reason...
                    if (starts_with(next_line, "; PA_CHANGE")) { // prune lines quickly before running pattern matching
                        std::size_t rc_pos = next_line.rfind("RC:");
                        if (rc_pos != std::string_view::npos) {
                            int rc_value = int(std::strtol(next_line.data() + rc_pos + 3, nullptr, 10));
                            if (rc_value == 1) {
                                break; // Role change found, stop searching
                            }
                        }
                    }
                    
                    // Found a Feedrate change command
                    // If the new feedrate is greater than any feedrate encountered so far after the PA change command, use that to calculate the PA value
                    // Also if this is the first feedrate we encounter, store it as the next feedrate.
                    if (starts_with(next_line, "G1 F")) { // prune lines quickly before running pattern matching
                        double feedrate = feedrate_of(next_line);
                        if(line_counter==1){ // this is the first command after the PA change pattern, and hence before any extrusion has happened. Reset
                                            // the current speed to this one
                            m_current_feedrate = feedrate;
                        }
                        if (temp_feed_rate < feedrate) {
                            temp_feed_rate = feedrate;
                        }
                        if(m_next_feedrate < EPSILON){ // This the first feedrate found after the PA Change command
                            m_next_feedrate = feedrate;
                        }
                        continue;
                    }
                }
                
                // If we found a new maximum feedrate after the PA change command, use it
                if (temp_feed_rate > 0) {
                    m_max_next_feedrate = temp_feed_rate;
                } else // If we didnt find a new feedrate at all after the PA change command, use the current feedrate.
                    m_max_next_feedrate = m_current_feedrate;

                
                // Calculate the predicted PA using the upcomming feature maximum feedrate
                // Get the interpolator for the active tool
                AdaptivePAInterpolator* interpolator = getInterpolator(m_last_config_index);
                
                double predicted_pa = 0;
                double adaptive_PA_speed = 0;
            
                if(!interpolator){ // Tool not found in the interpolator map
                    // Tool not found in the PA interpolator to tool map
                    predicted_pa = m_config.enable_pressure_advance.get_at(m_last_config_index) ? m_config.pressure_advance.get_at(m_last_config_index) : 0;
                    if(m_config.gcode_comments) output += "; APA: Tool doesnt have APA enabled\n";
                } else if (!interpolator->isInitialised() || (!m_config.adaptive_pressure_advance.get_at(m_last_config_index)) )
                    // Check if the model is not initialised by the constructor for the active extruder
                    // Also check that adaptive PA is enabled for that extruder. This should not be needed
                    // as the PA change flag should not be set upstream (in the GCode.cpp file) if adaptive PA is disabled
                    // however check for robustness sake.
                {
                    // Model failed or adaptive pressure advance not enabled - use default value from m_config
                    predicted_pa = m_config.enable_pressure_advance.get_at(m_last_config_index) ? m_config.pressure_advance.get_at(m_last_config_index) : 0;
                    if(m_config.gcode_comments) output += "; APA: Interpolator setup failed, using default pressure advance\n";
                } else { // Model setup succeeded
                    // Proceed to identify the print speed to use to calculate the adaptive PA value
                    if(isOverhang > 0){  // If we are in an overhang area, use the minimum between current print speed
                                        // and any speed immediately after
                                        // In most cases the current speed is the minimum one;
                                        // however if slowdown for layer cooling is enabled, the overhang
                                        // may be slowed down more than the current speed.
                        adaptive_PA_speed = (m_current_feedrate == 0 || m_next_feedrate == 0) ?
                                                std::max(m_current_feedrate, m_next_feedrate) :
                                                std::min(m_current_feedrate, m_next_feedrate);
                    }else{                // If this is not an overhang area, use the maximum speed from the current and
                                          // upcomming speeds for the island.
                        adaptive_PA_speed = std::max(m_max_next_feedrate,m_current_feedrate);
                    }
                    
                    // Calculate the adaptive PA value
                    predicted_pa = (*interpolator)(mm3mm_value * adaptive_PA_speed, accel_value);
                    
                    // This is a bridge, use the dedicated PA setting.
                    if(isBridge && m_config.adaptive_pressure_advance_bridges.get_at(m_last_config_index) > EPSILON)
                        predicted_pa = m_config.adaptive_pressure_advance_bridges.get_at(m_last_config_index);
                    
                    if (predicted_pa < 0) { // If extrapolation fails, fall back to the default PA for the extruder.
                        predicted_pa = m_config.enable_pressure_advance.get_at(m_last_config_index) ? m_config.pressure_advance.get_at(m_last_config_index) : 0;
                        if(m_config.gcode_comments) output += "; APA: Interpolation failed, using fallback pressure advance value\n";
                    }
                }
                if(m_config.gcode_comments) {
                    // Output debug GCode comments
                    output.append(pa_change_line) += '\n'; // Output PA change command tag
                    if(isBridge && m_config.adaptive_pressure_advance_bridges.get_at(m_last_config_index) > EPSILON)
                        output += "; APA Model Override (bridge)\n";
                    output += std::string("; APA Current Speed: ") + std::to_string(m_current_feedrate) + "\n";
                    output += std::string("; APA Next Speed: ") + std::to_string(m_next_feedrate) + "\n";
                    output += std::string("; APA Max Next Speed: ") + std::to_string(m_max_next_feedrate) + "\n";
                    output += std::string("; APA Speed Used: ") + std::to_string(adaptive_PA_speed) + "\n";
                    output += std::string("; APA Flow rate: ") + std::to_string(mm3mm_value * m_max_next_feedrate) + "\n";
                    output += std::string("; APA Prev PA: ") + std::to_string(m_last_predicted_pa) + " New PA: " + std::to_string(predicted_pa) + "\n";
                }
                if (config_index_changed || std::fabs(predicted_pa - m_last_predicted_pa) > EPSILON) {
                    output += m_gcodegen.writer().set_pressure_advance(predicted_pa); // Use m_writer to set pressure advance
                    m_last_predicted_pa = predicted_pa; // Update the last predicted PA value
                }
            }
        }else {
            // Output the current line as this isn't a PA change tag
            output.append(line) += '\n';
        }
    }

    return output;
}

std::string AdaptivePAProcessor::validate_adaptive_pa_model(const std::string& model_str)
{
    if (model_str.empty())
        return {}; // Empty model is valid
    
    std::istringstream model_stream(model_str);
    std::string line;
    int line_number = 0;
    
    while (std::getline(model_stream, line)) {
        ++line_number;
        
        // Trim whitespace
        const auto first = line.find_first_not_of(" \t\r\n");
        if (first == std::string::npos)
            continue; // Skip empty lines
        
        const auto last = line.find_last_not_of(" \t\r\n");
        line = line.substr(first, last - first + 1);
        
        // Only numbers, commas and dots are allowed (no letters or other characters)
        for (char c : line) {
            if (!std::isdigit(static_cast<unsigned char>(c)) && c != ',' && c != '.') {
                return "Line " + std::to_string(line_number) +
                       ": only numbers, commas and dots are allowed";
            }
        }

        // Count commas to validate format (should be exactly 2 for 3 values)
        int comma_count = 0;
        for (char c : line) {
            if (c == ',') comma_count++;
        }
        
        if (comma_count != 2) {
            return "Line " + std::to_string(line_number) + 
                   ": must contain exactly 3 comma-separated values (PA, flow, acceleration)";
        }
        
        // Parse and validate the values
        try {
            std::istringstream line_stream(line);
            std::string value;
            
            // Parse PA
            if (!std::getline(line_stream, value, ','))
                return "Line " + std::to_string(line_number) + ": missing PA value";
            double pa = std::stod(value);
            
            // Parse flow
            if (!std::getline(line_stream, value, ','))
                return "Line " + std::to_string(line_number) + ": missing flow value";
            double flow = std::stod(value);
            
            // Parse acceleration
            if (!std::getline(line_stream, value, ','))
                return "Line " + std::to_string(line_number) + ": missing acceleration value";
            double accel = std::stod(value);
            
            // Validate constraints
            if (pa >= 2.0) {
                return "Line " + std::to_string(line_number) + ": PA value must be less than 2";
            }
            if (flow <= pa) {
                return "Line " + std::to_string(line_number) + ": flow value must be greater than PA value";
            }
            if (accel <= flow) {
                return "Line " + std::to_string(line_number) + ": acceleration value must be greater than flow value";
            }
        } catch (const std::exception&) {
            return "Line " + std::to_string(line_number) + ": invalid numeric value";
        }
    }
    
    return {}; // All validations passed
}

} // namespace Slic3r
