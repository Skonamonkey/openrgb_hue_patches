/*---------------------------------------------------------*\
| PhilipsHueEntertainmentController.cpp                     |
|                                                           |
|   Detector for Philips Hue Entertainment Mode             |
|                                                           |
|   Adam Honse (calcprogrammer1@gmail.com)      06 Nov 2020 |
|                                                           |
|   This file is part of the OpenRGB project                |
|   SPDX-License-Identifier: GPL-2.0-only                   |
\*---------------------------------------------------------*/

#include "RGBController.h"
#include "PhilipsHueEntertainmentController.h"
#include "LogManager.h"

PhilipsHueEntertainmentController::PhilipsHueEntertainmentController(hueplusplus::Bridge& bridge_ptr, hueplusplus::Group group_ptr):bridge(bridge_ptr),group(group_ptr)
{
    /*-------------------------------------------------*\
    | Fill in location string with bridge IP            |
    \*-------------------------------------------------*/
    location                            = "IP: " + bridge.getBridgeIP();
    num_leds                            = (unsigned int)group.getLightIds().size();
    connected                           = false;
}

PhilipsHueEntertainmentController::~PhilipsHueEntertainmentController()
{

}

std::string PhilipsHueEntertainmentController::GetLocation()
{
    return(location);
}

std::string PhilipsHueEntertainmentController::GetName()
{
    return(group.getName());
}

std::string PhilipsHueEntertainmentController::GetVersion()
{
    return("");
}

std::string PhilipsHueEntertainmentController::GetManufacturer()
{
    return("");
}

std::string PhilipsHueEntertainmentController::GetUniqueID()
{
    return("");
}

unsigned int PhilipsHueEntertainmentController::GetNumLEDs()
{
    return(num_leds);
}

void PhilipsHueEntertainmentController::SetColor(RGBColor* colors)
{
    std::lock_guard<std::mutex> lock(connection_mutex);
    if(connected)
    {
        /*-------------------------------------------------*\
        | Fill in Entertainment Mode light data             |
        \*-------------------------------------------------*/
        for(unsigned int light_idx = 0; light_idx < num_leds; light_idx++)
        {
            RGBColor color                  = colors[light_idx];
            unsigned char red               = RGBGetRValue(color);
            unsigned char green             = RGBGetGValue(color);
            unsigned char blue              = RGBGetBValue(color);

            entertainment->setColorRGB(light_idx, red, green, blue);
        }

        entertainment->update();
    }
}

void PhilipsHueEntertainmentController::Connect()
{
    std::lock_guard<std::mutex> lock(connection_mutex);
    if(!connected)
    {
        /*-------------------------------------------------*\
        | Read and save current state of all lights so we   |
        | can restore it on disconnect and seed the first   |
        | entertainment packet to avoid a white flash       |
        \*-------------------------------------------------*/
        saved_states.clear();
        std::vector<int> light_ids = group.getLightIds();

        for(int light_id : light_ids)
        {
            HueSavedLightState state;
            try
            {
                hueplusplus::Light light = bridge.lights().get(light_id);
                light.refresh(true);
                state.on         = light.isOn();
                state.brightness = state.on ? light.getBrightness() : 0;
                state.xy         = light.getColorXY();
                state.has_xy     = (state.xy.xy.x > 0.f || state.xy.xy.y > 0.f);
            }
            catch(const std::exception& e)
            {
                LOG_WARNING("[PhilipsHue] Could not read light %d state: %s", light_id, e.what());
            }
            saved_states.push_back(state);
        }

        /*-------------------------------------------------*\
        | Create Entertainment Mode from bridge and group   |
        \*-------------------------------------------------*/
        entertainment = new hueplusplus::EntertainmentMode(bridge, group);

        /*-------------------------------------------------*\
        | Connect Hue Entertainment Mode                    |
        \*-------------------------------------------------*/
        if(entertainment->connect())
        {
            connected = true;

            /*---------------------------------------------*\
            | Immediately send a packet with the lights'    |
            | previous colours to suppress the white flash  |
            \*---------------------------------------------*/
            for(size_t i = 0; i < saved_states.size(); i++)
            {
                uint8_t r = 0, g = 0, b = 0;
                if(saved_states[i].on && saved_states[i].has_xy)
                {
                    hueplusplus::RGB rgb = hueplusplus::RGB::fromXY(saved_states[i].xy);
                    r = rgb.r;
                    g = rgb.g;
                    b = rgb.b;
                }
                entertainment->setColorRGB((uint8_t)i, r, g, b);
            }
            entertainment->update();
        }
        else
        {
            LOG_WARNING("[PhilipsHue] Entertainment Mode connect() failed");
            delete entertainment;
            entertainment = nullptr;
        }
    }
}

void PhilipsHueEntertainmentController::Disconnect()
{
    std::lock_guard<std::mutex> lock(connection_mutex);
    if(connected)
    {
        /*-------------------------------------------------*\
        | Disconnect Hue Entertainment Mode                 |
        \*-------------------------------------------------*/
        entertainment->disconnect();
        connected = false;

        delete entertainment;
        entertainment = nullptr;

        /*-------------------------------------------------*\
        | Restore each light to its pre-streaming state     |
        \*-------------------------------------------------*/
        std::vector<int> light_ids = group.getLightIds();

        for(size_t i = 0; i < light_ids.size() && i < saved_states.size(); i++)
        {
            try
            {
                hueplusplus::Light light = bridge.lights().get(light_ids[i]);

                if(saved_states[i].on)
                {
                    light.on(2);
                    light.setBrightness(saved_states[i].brightness, 2);

                    if(saved_states[i].has_xy)
                    {
                        light.setColorXY(saved_states[i].xy, 2);
                    }
                }
                else
                {
                    light.off(2);
                }
            }
            catch(const std::exception& e)
            {
                LOG_WARNING("[PhilipsHue] Could not restore light %d state: %s", light_ids[i], e.what());
            }
        }

        saved_states.clear();
    }
}
