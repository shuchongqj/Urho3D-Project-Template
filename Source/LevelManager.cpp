#include <Urho3D/UI/UI.h>
#include <Urho3D/IO/Log.h>
#include <Urho3D/Core/Context.h>
#include <Urho3D/Core/CoreEvents.h>
#include <Urho3D/Resource/Localization.h>
#include <Urho3D/Engine/DebugHud.h>
#include <Urho3D/Resource/ResourceCache.h>
#include <Urho3D/Graphics/Graphics.h>
#include "LevelManager.h"
#include "Levels/Splash.h"
#include "Levels/MainMenu.h"
#include "Levels/Level.h"
#include "Levels/ExitGame.h"
#include "Levels/Loading.h"
#include "Levels/Credits.h"
#include "Console/ConsoleHandlerEvents.h"
#include "LevelManagerEvents.h"
#include "UI/WindowEvents.h"

using namespace Urho3D;
using namespace ConsoleHandlerEvents;
using namespace LevelManagerEvents;
using namespace WindowEvents;

LevelManager::LevelManager(Context* context) :
Object(context),
currentLevel_("NONE")
{

    // Listen to set level event
    SubscribeToEvent(E_SET_LEVEL, URHO3D_HANDLER(LevelManager, HandleSetLevelQueue));

    if (GetSubsystem<UI>()) {
        GetSubsystem<UI>()->GetRoot()->RemoveAllChildren();
    }

    // How to use lambda (anonymous) functions
    SendEvent(E_CONSOLE_COMMAND_ADD, ConsoleCommandAdd::P_NAME, "change_level", ConsoleCommandAdd::P_EVENT, "ChangeLevelConsole", ConsoleCommandAdd::P_DESCRIPTION, "Change level");
    SubscribeToEvent("ChangeLevelConsole", [&](StringHash eventType, VariantMap& eventData) {
        StringVector params = eventData["Parameters"].GetStringVector();

        if (params.Size() != 2) {
            URHO3D_LOGERROR("Invalid number of parameters!");
        } else {
            VariantMap& data = GetEventDataMap();
            data[SetLevel::P_NAME] = params[1];
            SendEvent(E_SET_LEVEL, data);
        }
    });
}

LevelManager::~LevelManager()
{
}

void LevelManager::RegisterObject(Context* context)
{
    context->RegisterFactory<LevelManager>();

    Levels::Splash::RegisterObject(context);
    Levels::Level::RegisterObject(context);
    Levels::MainMenu::RegisterObject(context);
    Levels::ExitGame::RegisterObject(context);
    Levels::Loading::RegisterObject(context);
    Levels::Credits::RegisterObject(context);

}

void LevelManager::HandleSetLevelQueue(StringHash eventType, VariantMap& eventData)
{
    // Busy now
    if (level_queue_.Size() == 0) {
        // Init fade status
        fade_status_ = 0;
    }

    if (eventData["Name"].GetString().Empty()) {
        URHO3D_LOGERROR("Level manager failed to push level to queue, level name empty!");
        return;
    }
    // Push to queue
    level_queue_.Push(eventData["Name"].GetString());
    data_ = eventData;

    // Subscribe HandleUpdate() function for processing update events
    SubscribeToEvent(E_UPDATE, URHO3D_HANDLER(LevelManager, HandleUpdate));
}

void LevelManager::HandleUpdate(StringHash eventType, VariantMap& eventData)
{
    using namespace Update;

    // Take the frame time step, which is stored as a float
    float timeStep = eventData[P_TIMESTEP].GetFloat();

    // Move sprites, scale movement with time step
    fade_time_ -= timeStep;

    // Prepare to fade out
    if (fade_status_ == 0) {
        using namespace LevelChangingStarted;
        VariantMap& data = GetEventDataMap();
        data[P_FROM] = currentLevel_;
        data[P_TO] = level_queue_.Front();
        SendEvent(E_LEVEL_CHANGING_STARTED, data);

        // No old level
        if (!level_) {
            fade_status_++;
            return;
        }
        // Add a new fade layer
        AddFadeLayer();
        fade_window_->SetOpacity(0.0f);
        fade_time_ = MAX_FADE_TIME;
        fade_status_++;

        return;
    }

    // Fade out
    if (fade_status_ == 1) {
        // No old level
        if (!level_) {
            fade_status_++;
            return;
        }
        fade_window_->SetFocus(true);
        fade_window_->SetOpacity(1.0f - fade_time_ / MAX_FADE_TIME);

        // Increase fade status
        if (fade_time_ <= 0.0f) {
            fade_status_++;
            SendEvent(E_LEVEL_BEFORE_DESTROY);
        }
        return;
    }

    // Release old level
    if (fade_status_ == 2) {
        // We can not create new level here, or it may cause errors, we have to create it at the next update point.
        level_ = SharedPtr<Object>();
        fade_status_++;

        // Send event to close all active UI windows
        SendEvent(E_CLOSE_ALL_WINDOWS);
        return;
    }

    // Create new level
    if (fade_status_ == 3) {
        // Create new level
        level_ = context_->CreateObject(StringHash(level_queue_.Front()));
        if (!level_) {
            URHO3D_LOGERROR("Level '" + level_queue_.Front() + "' doesn't exist in the system! Moving to 'MainMenu' level");

            auto* localization = GetSubsystem<Localization>();
            VariantMap& eventData = GetEventDataMap();
            eventData["Name"] = "MainMenu";
            eventData["Message"] = localization->Get("LEVEL_NOT_EXIST") + " : " + level_queue_.Front();
            SendEvent(E_SET_LEVEL, eventData);

            level_queue_.PopFront();
            return;
        }

        previousLevel_ = currentLevel_;
        currentLevel_ = level_queue_.Front();
        SetGlobalVar("CurrentLevel", currentLevel_);

        if (GetSubsystem<DebugHud>()) {
            GetSubsystem<DebugHud>()->SetAppStats("Current level", currentLevel_);
        }

        // Add a new fade layer
        AddFadeLayer();
        fade_window_->SetOpacity(1.0f);
        fade_time_ = MAX_FADE_TIME;
        fade_status_++;

        using namespace LevelChangingInProgress;
        data_[P_FROM] = previousLevel_;
        data_[P_TO] = currentLevel_;
        SendEvent(E_LEVEL_CHANGING_IN_PROGRESS, data_);
        return;
    }

    // Fade in
    if (fade_status_ == 4) {
        fade_window_->SetFocus(true);
        fade_window_->SetOpacity(fade_time_ / MAX_FADE_TIME);

        // Increase fade status
        if (fade_time_ <= 0.0f) {
            fade_status_++;
        }
        return;
    }

    // Finished
    if (fade_status_ == 5) {
        // Remove fade layer
        fade_window_->Remove();
        fade_window_.Reset();
        // Unsubscribe update event
        UnsubscribeFromEvent(E_UPDATE);

        {
            using namespace LevelChangingFinished;
            VariantMap& data = GetEventDataMap();
            data[P_FROM] = previousLevel_;
            data[P_TO] = level_queue_.Front();
            SendEvent(E_LEVEL_CHANGING_FINISHED, data);
        }

        VariantMap& data = GetEventDataMap();
        data["Name"] = level_queue_.Front();
        SendEvent("LevelLoaded", data);

        // Remove the task
        level_queue_.PopFront();

        // Release all unused resources
        GetSubsystem<ResourceCache>()->ReleaseAllResources(false);

        if (level_queue_.Size()) {
            // Subscribe HandleUpdate() function for processing update events
            SubscribeToEvent(E_UPDATE, URHO3D_HANDLER(LevelManager, HandleUpdate));

            // // Init fade status
            fade_status_ = 0;
        }
        return;
    }
}

void LevelManager::AddFadeLayer()
{
    if (fade_window_) {
        fade_window_.Reset();
    }
    fade_window_ = new Window(context_);
    if (GetSubsystem<Graphics>()) {
        // Make the window a child of the root element, which fills the whole screen.
        GetSubsystem<UI>()->GetRoot()->AddChild(fade_window_);
        fade_window_->SetSize(GetSubsystem<Graphics>()->GetWidth(), GetSubsystem<Graphics>()->GetHeight());
        fade_window_->SetLayout(LM_FREE);
        // Urho has three layouts: LM_FREE, LM_HORIZONTAL and LM_VERTICAL.
        // In LM_FREE the child elements of this window can be arranged freely.
        // In the other two they are arranged as a horizontal or vertical list.

        // Center this window in it's parent element.
        fade_window_->SetAlignment(HA_CENTER, VA_CENTER);
        // Black color
        fade_window_->SetColor(Color(0.0f, 0.0f, 0.0f, 1.0f));
        // Make it topmost
        fade_window_->BringToFront();
        fade_window_->SetPriority(1000);
    }
}
