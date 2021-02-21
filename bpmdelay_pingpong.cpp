/*
 * File: bpmdelay_pingpong.cpp
 *
 * A simple BPM sync'd PingPong style delay for the NTS-1
 * 
 * Note, this delay uses the dry/wet (shift-B) to set the wet / dry ratio
 * 
 * 
 * hammondeggsmusic.ca 2021
 *
 */

#include "userdelfx.h" 


// Defines
#define NUM_DELAY_DIVISIONS      15       // # of bpm divisions in table
#define DELAY_LINE_SIZE          0x40000  // Delay line size (*must be a power of 2)
#define DELAY_LINE_SIZE_MASK     0x3FFFF  // Mask for the delay line size for rollover
#define DELAY_GLIDE_RATE         12000    //  this value must not be lower than 1. larger values = slower glide rates for delay time
#define MIN_BPM                  56       // failsafe, likely never used
#define NUM_NOTES_PER_BEAT       4        // The xd/prologue use quarter notes, hence '4'.
#define SAMPLE_RATE              48000    // 48KHz is our fixed sample rate (the const k_samplerate is only listed in the osc_api.h not the fx_api.h)


#define PSEUDO_STEREO_OFFSET (float)SAMPLE_RATE * .01f    // How much time to offset the right channel in seconds for pseudo stereo(.01 = 10ms) 

// Delay BPM division with time knob from 0 to full:
// 1/64, 1/48, 1/32, 1/24, 1/16, 1/12, 1/8, 1/6, 3/16, 1/4, 1/3, 3/8, 1/2, 3/4, 1
float delayDivisions[NUM_DELAY_DIVISIONS] = 
{0.015625,.02083333,.03125,.04166666,.0625f,.08333333f,.125f,.16666667f,.1875f,.25f,.33333333f,.375f,.5f,.75f,1};

// Delay lines for left / right channel
__sdram float delayLine_L[DELAY_LINE_SIZE];
__sdram float delayLine_R[DELAY_LINE_SIZE];

// Current position in the delay line we are writing to:
// (integer value as it is per-sample)
uint32_t delayLine_Wr = 0;

// Smoothing (glide) for delay time:
// This is the current delay time as we smooth it
float currentDelayTime = 48000; 

// This is the delay time we actually wish to set to
float targetDelayTime = 48000;

// Depth knob value from 0-1
float valDepth = 0;

// Time value knob from 0-1
float valTime = 0;

// Delay time multiplier (will be pulled from delayDivisions table)
float multiplier = 1;

// Wet/Dry signal levels
float wet = .5;
float dry = .5;

 
////////////////////////////////////////////////////////////////////////
// DELFX_INIT
// - initialize the effect variables, including clearing the delay lines
////////////////////////////////////////////////////////////////////////
void DELFX_INIT(uint32_t platform, uint32_t api)
{
   // Initialize the variables used
   delayLine_Wr = 0;

   // Clear the delay lines. If you don't do this, it is entirely possible that "something" will already be there, and you might
   // get either old delay sounds, or very unpleasant noises from a previous effects. 
   for (int i=0;i<DELAY_LINE_SIZE;i++)
   {
      delayLine_L[i] = 0;
      delayLine_R[i] = 0;
   }

   
   currentDelayTime = SAMPLE_RATE; 
   targetDelayTime = SAMPLE_RATE;

   valDepth = 0;
   valTime = 0;
   multiplier = 1;


   wet = 0.5f;
   dry = 0.5f;
   
}


////////////////////////////////////////////////////////////////////////////////////////////////////////
// readFrac
// 
// fractionally read from a buffer
// That is, this allows you to read 'between' two points in a table
// using a floating point index.
//  - buffer size must be a power of 2
//
// this is from the korg example (slightly modified)
////////////////////////////////////////////////////////////////////////////////////////////////////////
// compiler conditions to a: compile this code 'inline' and b: set a specific optimization for this routine.
// compiling inline saves you a few cycles but can result in larger code.
inline __attribute__((optimize("Ofast"),always_inline)) 
float readFrac(const float pos, float *pDelayLine) 
{
   // Get the 'base' value - that is, the integer value of the position
   // e.g. if we're looking for value at position 1423.6, this will yield an integer of 1426
   uint32_t base = (uint32_t)pos;

   // Get the fraction (decimal) portion of the index
   const float frac = pos - base;	

   // Get the sample at the base index - note by masking the base index with the delay line mask we don't have
   // to do any modulus / manual checks for overflow. This requries the buffer size to be a power of 2.
   const float s0 = pDelayLine[base & DELAY_LINE_SIZE_MASK];

   // Get the next sample at the base index + 1. Again, by masking with the delay line size mask, we don't have 
   // to worry about rolling over the buffer index.
   base++;
   const float s1 = pDelayLine[base & DELAY_LINE_SIZE_MASK];

   // Using the logue-sdk linear interpolation function, get the linearly-interpolated result of the two sample values.
   float r = linintf(frac, s0, s1);
   return r;    
}


 




////////////////////////////////////////////////////////////////////////
// DELFX_PROCESS
// - Called for every buffer , process your samples here
////////////////////////////////////////////////////////////////////////
void DELFX_PROCESS(float *xn, uint32_t frames)
{

   float * __restrict x = xn; // Local pointer, pointer xn copied here. 
   const float * x_e = x + 2*frames; // End of data buffer address


   // *Any code here will be called ONCE per buffer. Typically there are 16 samples per buffer,
   // but there is no reason this could not be more - or less.
   // Get the BPM value here (it won't change (or if it does it won't matter terribly much...) 
   // during the sample process loop below so no need to keep calling this
   // while processing samples, saves some cpu time.)
   float bpmF = fx_get_bpmf(); //this is the bpm, in minutes


   // Failsafe - since we are going to divide by bpmF it can never be zero. 
   // It never is, but a good idea in my opinion to make sure.
   if (bpmF <= 0)
   {
      // failsafe, set to a known safe value.
      bpmF = MIN_BPM;
   }

   
   //Calculate the # of beats per second 
   float bpm_s = 60 / bpmF;

   // Calculate our delay time (as a float) by taking:
   //   The # of samples per second * the # of beats per second * the number of notes per second * our multiplier.
   //   note, the multiplier is 1 or lower, so this will result in a reduction only.
   targetDelayTime = SAMPLE_RATE * bpm_s * NUM_NOTES_PER_BEAT * multiplier;
          
   // Loop through the samples - for delay effects, you replace the value at *xn with your new value
   // This data is interleaved with left/right data
   for (; x != x_e; ) 
   {
      // Smoothly transition the delay time
      // - This gives the same effect as exponential 'glide'

      // Calculate the difference between the target and the current delay time
      float delta = targetDelayTime - currentDelayTime;

      // Divide this by the glide rate (larger glide rates = longer glide times.)
      // Glide rate cannot be lower than 1!
      delta /= DELAY_GLIDE_RATE;

      // Add to our current delay time this delta. 
      currentDelayTime += delta;   

      //Get our input signal values to the effect

      float sigInL = *x; // get the value pointed at x (Left channel)
      float sigInR = *(x+1); // get the value pointed at x + 1(right channel)
      
      // Declare some storage for our output signals
      float sigOutL;
      float sigOutR;

      // The way this delay will work, is we will continually write to the delay line
      // with the new incoming audio directly into the delay line (per sample). 
      // We will read 'behind' this index using a floating point value to allow us
      // to read sub-sample values from this delay line.

      // Calculate the read index (floating point so can have a fraction)
      float readIndex = (float)delayLine_Wr - currentDelayTime;

      // Since this is a float we can't just mask it to account for rollover
      // - since we subtracted the index it could be negative - roll this value over
      // around the delay line
      if (readIndex < 0)
      {
         // Roll the pointer back to the end of the buffer (index)
         readIndex += DELAY_LINE_SIZE_MASK;
      }

      // Ping-pong style delay:
      // Read the delayed (behind) signal for the right channel first
      float delayLineSig_R = readFrac(readIndex, delayLine_R);

      // Write the right channel input signal into the right channel buffer
      delayLine_R[delayLine_Wr] = sigInR;

      // Store the delayed right channel signal - multiplied by the feedback value (0-1) into the left channel
      delayLine_L[delayLine_Wr] = delayLineSig_R * valDepth; //tbd on the valdepth

      // Read the delayed (behind) signal for the left channel
      float delayLineSig_L = readFrac(readIndex, delayLine_L);

      // *Add* (mix) this signal with the existing signal at the right channel delay line (multiplied by feedback)
      // - that is, effectively mix this left delayed signal with the right input signal 
      delayLine_R[delayLine_Wr] += delayLineSig_L * valDepth;

      // Increment and roll over our write index for the delay line 
      // This is an integer, and a power of 2 so we can simply mask the value by the DELAY_LINE_SIZE_MASK.
      delayLine_Wr++;
      delayLine_Wr &= DELAY_LINE_SIZE_MASK; 

    
      // Generate our output signal:
      // That is, the input signal * the dry level + (mixed with) the delayed signal * the wet level.
      sigOutL = sigInL * dry + delayLineSig_L * wet;

      // And again for the right channel
      sigOutR = sigInR * dry + delayLineSig_R * wet;

      // Store this result into the output buffer
      *x = sigOutL;

      // Move to the next channel
      x++;

      // Store this result into the output buffer
      *x = sigOutR; 

      // Move to the next interleaved sample
      x++;		
   }
   
}



////////////////////////////////////////////////////////////////////////////////////
//		PARAM
//
// On changing any of the knobs this is called. If you have any math to perform
// on the parameters, do it here vs in the audio callback to save a ton of time
// BPM is NOT sent here - you have to pull it manually via:
// fx_get_bpmf() for float or
// fx_get_bpm() for integer * 10 
//
// We'll use fx_get_bpmf(), which returns a floating point value.
//
// If there are values to be calculated based on these knob values, it is ideal to 
// put those calculations in here and not in the DSP loop as you will be wasting 
// CPU time calculating those values every time. 
//
////////////////////////////////////////////////////////////////////////////////////
void DELFX_PARAM(uint8_t index, int32_t value)
{
   
   // Convert the int32_t (q31 fixed point) value we're given to a float from 0-1
   // Note - if you wish to declare variables to use in the switch statement, you may
   // need to declare them here, the compiler may throw an error if you try to declare
   // it within the case statements etc. 
   const float valf = q31_to_f32(value);
   float f; // Temp value used for calculations
   float s_mix; // Used for wet/dry calculations
   int divIndex; // Integer value for delay time multiplier (division really as the value is 1 or <1) table index
   // Select which parameter to work with:
   switch (index) 
   {
      case k_user_delfx_param_time:
         ////////////////////////////
         // "A" / TIME KNOB
         ////////////////////////////
         // Calculate the coarse delay time (via the divisions table)

         //Store this 0-1 value in case we need it for something else (currently we do not)
         valTime = valf;

         // Convert the 0-1 value into an array index (e.g. there are 15 divisions, so we need an index value from 0-14)         
         f = valf;
         f *= NUM_DELAY_DIVISIONS - 1; //0-14 15 delay divisions

         // Set an integer value for our table index
         divIndex = (int)f;
         
         // Failsafe, ensure it is within a valid range (it always is though)
         if ((divIndex >= NUM_DELAY_DIVISIONS) || (divIndex < 0))
         {
            divIndex = NUM_DELAY_DIVISIONS - 1;//failsafe
         }

         // Get the time multiplier from the division table.
         multiplier = delayDivisions[divIndex];
         break;

      case k_user_delfx_param_depth:      
         ////////////////////////////
         // "B" / DEPTH KNOB
         ////////////////////////////
         // Set the delay feedback (0-1, tbd if i use an exp table)   
         // Just store this value for the DSP loop to use.
         valDepth = valf;
         break;

      case k_user_delfx_param_shift_depth:         
         ////////////////////////////////
         // "DELAY+B" / SHIFT-DEPTH KNOB
         ////////////////////////////////
         // For delays this is wet/dry, though you can technically use this 3rd parameter for whatever you want!
         //Left side of the knob (all dry to full mix)
         // Adapted from the korg example, this allows us to get a 50/50 split at full mix but a higher level for
         // full wet / full dry. 

         // I've expanded the korg example here to make it a bit easier to follow
         //s_mix = (valf <= 0.49f) ? 1.02040816326530612244f * valf : (valf >= 0.51f) ? 0.5f + 1.02f * (valf-0.51f) : 0.5f;    
         
         // Are we at the left half of the knob position?
         if (valf <= 0.49f)
         {
            // Yes, amplify the mix value slightly
            s_mix = 1.02040816326530612244f * valf;
         }
         // No, are we at the right half of the knob position?
         else if (valf >= 0.51f) 
         {
            // Yes, amplify the mix value but also invert it and subtract the 0.51 offset from it (so the mix value will decrease when turning the knob higher)
            s_mix = 0.5f + 1.02f * (valf-0.51f);
         }
         else
         {
            // Midpoint, set the mix value to 50%
            s_mix =  0.5f; 
         }
         // Calculate our wet / dry values
         dry = 1.0f - s_mix; 
         wet = s_mix;         
         break;

      default:
         // no default handling, there is no case for this.         
         break;
   }
}












