#pragma once

// ============================================================================
// DspDebugLog.h — Simple file-based logger for CAB-TR diagnostics
//
// Writes IR loading events and parameter changes to a text file on Desktop.
// Thread-safe with mutex protection.
//
// Usage:
//   #define CABTR_DSP_DEBUG_LOG 1   // enable logging (0 = no overhead)
//
//   LOG_IR_EVENT("NORMALIZE applied: maxLevel=0.5, gain=2.0x (+6.0dB)");
//   LOG_IR_EVENT("REVERSE applied to IR");
//   LOG_IR_EVENT("IR loaded: " + filename);
//
// Output file: Desktop/CAB-TR_DebugLog.txt
// ============================================================================

#include <JuceHeader.h>
#include <mutex>

#ifndef CABTR_DSP_DEBUG_LOG
 #define CABTR_DSP_DEBUG_LOG 0
#endif

#if CABTR_DSP_DEBUG_LOG

class DspDebugLog
{
public:
    static DspDebugLog& getInstance()
    {
        static DspDebugLog instance;
        return instance;
    }

    void log (const juce::String& message)
    {
        std::lock_guard<std::mutex> lock (mutex);
        
        if (! logFile.existsAsFile())
            initLogFile();
        
        if (logFile.existsAsFile())
        {
            const juce::String timestamp = juce::Time::getCurrentTime().toString (true, true, true, true);
            const juce::String line = timestamp + " | " + message + "\n";
            logFile.appendText (line, false, false);
        }
    }

    void clear()
    {
        std::lock_guard<std::mutex> lock (mutex);
        if (logFile.existsAsFile())
            logFile.deleteFile();
        initLogFile();
    }

private:
    DspDebugLog()
    {
        initLogFile();
    }

    void initLogFile()
    {
        // Create log file on Desktop
        auto desktop = juce::File::getSpecialLocation (juce::File::userDesktopDirectory);
        logFile = desktop.getChildFile ("CAB-TR_DebugLog.txt");
        
        if (! logFile.existsAsFile())
        {
            logFile.create();
            const juce::String header = "=================================================\n"
                                       "CAB-TR Debug Log\n"
                                       "=================================================\n\n";
            logFile.appendText (header, false, false);
        }
    }

    juce::File logFile;
    std::mutex mutex;
};

#define LOG_IR_EVENT(msg) DspDebugLog::getInstance().log(msg)
#define CLEAR_IR_LOG() DspDebugLog::getInstance().clear()

#else

#define LOG_IR_EVENT(msg) 
#define CLEAR_IR_LOG()

#endif

