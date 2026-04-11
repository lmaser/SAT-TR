#pragma once

// ============================================================================
// DspDebugLog.h — Simple file-based logger for SAT-TR diagnostics
//
// Writes DSP events and parameter changes to a text file on Desktop.
// Thread-safe with mutex protection.
//
// Usage:
//   #define SATTR_DSP_DEBUG_LOG 1   // enable logging (0 = no overhead)
//
//   LOG_DSP_EVENT("NORMALIZE applied: maxLevel=0.5, gain=2.0x (+6.0dB)");
//   LOG_DSP_EVENT("ALIGN applied");
//   LOG_DSP_EVENT("Loader A updated");
//
// Output file: Desktop/SAT-TR_DebugLog.txt
// ============================================================================

#include <JuceHeader.h>
#include <mutex>

#ifndef SATTR_DSP_DEBUG_LOG
 #define SATTR_DSP_DEBUG_LOG 0
#endif

#if SATTR_DSP_DEBUG_LOG

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
        logFile = desktop.getChildFile ("SAT-TR_DebugLog.txt");
        
        if (! logFile.existsAsFile())
        {
            logFile.create();
            const juce::String header = "=================================================\n"
                                       "SAT-TR Debug Log\n"
                                       "=================================================\n\n";
            logFile.appendText (header, false, false);
        }
    }

    juce::File logFile;
    std::mutex mutex;
};

#define LOG_DSP_EVENT(msg) DspDebugLog::getInstance().log(msg)
#define CLEAR_DSP_LOG() DspDebugLog::getInstance().clear()

#else

#define LOG_DSP_EVENT(msg) 
#define CLEAR_DSP_LOG()

#endif

