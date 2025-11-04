// Georgy Treshchev 2024.

#include "VAD/RuntimeVoiceActivityDetector.h"

#include "RuntimeAudioImporterDefines.h"
#include "RuntimeAudioImporterTypes.h"
#include "VADIncludes.h"
#include "HAL/UnrealMemory.h"
#include "Codecs/RAW_RuntimeCodec.h"
#if !UE_VERSION_OLDER_THAN(5, 1, 0)
#include "DSP/FloatArrayMath.h"
#endif

URuntimeVoiceActivityDetector::URuntimeVoiceActivityDetector()
	: AppliedSampleRate(0)
	  , MinimumSpeechDuration(300) // 300ms default minimum speech duration
	  , SilenceDuration(500) // 500ms default silence duration
	  , bIsSpeechActive(false)
	  , ConsecutiveVoiceFrames(0)
	  , ConsecutiveSilenceFrames(0)
	  , FrameDurationMs(0)
{
#if WITH_RUNTIMEAUDIOIMPORTER_VAD_SUPPORT
	SileroVAD = CreateDefaultSubobject<URuntimeSileroVADProvider>(TEXT("SileroVAD"));
	if (SileroVAD)
	{
		UE_LOG(LogRuntimeAudioImporter, VeryVerbose, TEXT("Successfully created VAD instance for %s"), *GetName());
		SetVADMode(ERuntimeVADMode::VeryAggressive);
	}
	else
	{
		UE_LOG(LogRuntimeAudioImporter, Error, TEXT("Unable to create VAD instance for %s"), *GetName());
	}
#else
    if (!HasAnyFlags(RF_ClassDefaultObject))
    {
		UE_LOG(LogRuntimeAudioImporter, Error, TEXT("VAD support is disabled, unable to create VAD instance for %s"), *GetName());
	}
#endif
}

void URuntimeVoiceActivityDetector::BeginDestroy()
{
	UObject::BeginDestroy();
}

bool URuntimeVoiceActivityDetector::ResetVAD()
{
#if WITH_RUNTIMEAUDIOIMPORTER_VAD_SUPPORT
	if (!IsValid(SileroVAD))
	{
		UE_LOG(LogRuntimeAudioImporter, Error, TEXT("Unable to reset VAD for %s as the VAD instance is not valid"), *GetName());
		return false;
	}
	SetVADMode(ERuntimeVADMode::VeryAggressive);
	AppliedSampleRate = 0;
	bIsSpeechActive = false;
	ConsecutiveVoiceFrames = 0;
	ConsecutiveSilenceFrames = 0;
	UE_LOG(LogRuntimeAudioImporter, Log, TEXT("Successfully reset VAD for %s"), *GetName());
	return true;
#else
	UE_LOG(LogRuntimeAudioImporter, Error, TEXT("Unable to reset VAD for %s as VAD support is disabled"), *GetName());
	return false;
#endif
}

bool URuntimeVoiceActivityDetector::SetVADMode(ERuntimeVADMode Mode)
{
	UE_LOG(LogRuntimeAudioImporter, Log, TEXT("Setting VAD mode is unimplemented."));
	return false;
	/*
#if WITH_RUNTIMEAUDIOIMPORTER_VAD_SUPPORT
	if (!VADInstance)
	{
		UE_LOG(LogRuntimeAudioImporter, Error, TEXT("Unable to set VAD mode for %s as the VAD instance is not valid"), *GetName());
		return false;
	}
	if (FVAD_RuntimeAudioImporter::fvad_set_mode(VADInstance, VoiceActivityDetector::GetVADModeInt(Mode)) != 0)
	{
		UE_LOG(LogRuntimeAudioImporter, Error, TEXT("Unable to set VAD mode for %s as the mode is invalid"), *GetName());
		return false;
	}
	UE_LOG(LogRuntimeAudioImporter, Log, TEXT("Successfully set VAD mode for %s to %s"), *GetName(), *UEnum::GetValueAsName(Mode).ToString());
	return true;
#else
	UE_LOG(LogRuntimeAudioImporter, Error, TEXT("Unable to set VAD mode for %s as VAD support is disabled"), *GetName());
	return false;
#endif
	*/
}

bool URuntimeVoiceActivityDetector::ProcessVAD(TArray<float> PCMData, int32 InSampleRate, int32 NumOfChannels)
{
#if WITH_RUNTIMEAUDIOIMPORTER_VAD_SUPPORT
	if (!IsValid(SileroVAD))
	{
		UE_LOG(LogRuntimeAudioImporter, Error, TEXT("Unable to process VAD for %s as the VAD instance is not valid"), *GetName());
		return false;
	}
	if (PCMData.Num() == 0)
	{
		UE_LOG(LogRuntimeAudioImporter, Error, TEXT("Unable to process VAD for %s as the PCM data is empty"), *GetName());
		return false;
	}
	if (InSampleRate <= 0)
	{
		UE_LOG(LogRuntimeAudioImporter, Error, TEXT("Unable to process VAD for %s as the sample rate is invalid"), *GetName());
		return false;
	}
	if (NumOfChannels <= 0)
	{
		UE_LOG(LogRuntimeAudioImporter, Error, TEXT("Unable to process VAD for %s as the number of channels is invalid"), *GetName());
		return false;
	}

	Audio::FAlignedFloatBuffer AlignedPCMData;
	AlignedPCMData = MoveTemp(PCMData);

	// Mix channels if necessary (VAD only supports mono audio data)
	if (NumOfChannels > 1)
	{
		Audio::FAlignedFloatBuffer AlignedPCMData_Mixed;
		if (!FRAW_RuntimeCodec::MixChannelsRAWData(AlignedPCMData, InSampleRate, NumOfChannels, 1, AlignedPCMData_Mixed))
		{
			UE_LOG(LogRuntimeAudioImporter, Error, TEXT("Unable to mix audio data for %s"), *GetName());
			return false;
		}
		AlignedPCMData = MoveTemp(AlignedPCMData_Mixed);
	}

	// Resample the audio data if necessary (Silero only supports 16 kHz audio data)
	static constexpr int32 VADTargetSampleRate = 16000;
	if (InSampleRate != VADTargetSampleRate)
	{
		Audio::FAlignedFloatBuffer AlignedPCMData_Resampled;
		if (!FRAW_RuntimeCodec::ResampleRAWData(AlignedPCMData, 1, InSampleRate, VADTargetSampleRate, AlignedPCMData_Resampled))
		{
			UE_LOG(LogRuntimeAudioImporter, Error, TEXT("Unable to resample audio data for %s"), *GetName());
			return false;
		}

		AlignedPCMData = MoveTemp(AlignedPCMData_Resampled);
		InSampleRate = VADTargetSampleRate;
		UE_LOG(LogRuntimeAudioImporter, Verbose, TEXT("Successfully resampled audio data for %s to %d sample rate"), *GetName(), VADTargetSampleRate);
	}
	
	AppliedSampleRate = InSampleRate;

	// Append the new PCM data to the accumulated data
	AccumulatedPCMData.Append(MoveTemp(AlignedPCMData));

	// Calculate the length of the accumulated audio data in milliseconds
	float AudioDataLengthMs = static_cast<float>(AccumulatedPCMData.Num()) / static_cast<float>(AppliedSampleRate) * 1000;

	// Process the accumulated audio data if it reaches 10, 20, or 30 ms (VAD only supports these frame lengths)
	if (AudioDataLengthMs >= 10)
	{
		int32 ValidLength = [this, AudioDataLengthMs]()
		{
			if (AudioDataLengthMs >= 30)
			{
				return 30;
			}
			else if (AudioDataLengthMs >= 20)
			{
				return 20;
			}
			else if (AudioDataLengthMs >= 10)
			{
				return 10;
			}
			UE_LOG(LogRuntimeAudioImporter, Error, TEXT("Unable to process VAD for %s as the audio data length is invalid"), *GetName());
			return 0;
		}();

		// Calculate the number of samples to process
		int32 NumToProcess = ValidLength * AppliedSampleRate / 1000;

		// Process the VAD
		int32 VADResult = SileroVAD->ProcessAudio(AccumulatedPCMData, AppliedSampleRate, NumToProcess); //Previously: FVAD_RuntimeAudioImporter::fvad_process(VADInstance, AccumulatedPCMData.GetData(), NumToProcess);

		// Remove processed data from the accumulated buffer
		AccumulatedPCMData.RemoveAt(0, NumToProcess);
		FrameDurationMs = ValidLength;

		if (VADResult == 1)
		{
			ConsecutiveVoiceFrames += ValidLength;
			ConsecutiveSilenceFrames = 0;

			// Check if speech should start
			if (!bIsSpeechActive && ConsecutiveVoiceFrames >= MinimumSpeechDuration)
			{
				bIsSpeechActive = true;
				OnSpeechStartedNative.Broadcast();
				OnSpeechStarted.Broadcast();
				UE_LOG(LogRuntimeAudioImporter, Log, TEXT("Speech started for %s"), *GetName());
			}
			
			UE_LOG(LogRuntimeAudioImporter, Verbose, TEXT("VAD detected voice activity for %s"), *GetName());
			return true;
		}
		else if (VADResult == 0)
		{
			ConsecutiveVoiceFrames = 0;
			ConsecutiveSilenceFrames += ValidLength;

			// Check if speech should end
			if (bIsSpeechActive && ConsecutiveSilenceFrames >= SilenceDuration)
			{
				bIsSpeechActive = false;
				OnSpeechEndedNative.Broadcast();
				
				AsyncTask(ENamedThreads::GameThread, [WeakThis = MakeWeakObjectPtr(this)]() mutable
				{
						WeakThis->OnSpeechEnded.Broadcast();
				});
				
				UE_LOG(LogRuntimeAudioImporter, Log, TEXT("Speech ended for %s"), *GetName());
			}

			UE_LOG(LogRuntimeAudioImporter, Verbose, TEXT("VAD detected no voice activity for %s"), *GetName());
			return false;
		}
		else
		{
			UE_LOG(LogRuntimeAudioImporter, Error, TEXT("Unable to process VAD for %s due to %d error code"), *GetName(), VADResult);
			return false;
		}
	}
	else
	{
		UE_LOG(LogRuntimeAudioImporter, Verbose, TEXT("Accumulating audio data until it reaches 10, 20 or 30 ms for %s. Current length: %f ms"), *GetName(), AudioDataLengthMs);
		return false;
	}
#else
	UE_LOG(LogRuntimeAudioImporter, Error, TEXT("Unable to process VAD for %s as VAD support is disabled"), *GetName());
	return false;
#endif
}
