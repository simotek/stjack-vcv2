#include "jack-audio-module.hh"
#include "hashids.hh"

#include <algorithm>

// NOTE: The AUDIO_OUTPUTS and AUDIO_INPUTS constants have mostly lost
// their meaning through several updates. If either is changed from 4
// then stupidity will occur. Likely we should just remove them since
// they are now unhelpful and will inevitably give someone the
// impression that they can be changed.

void JackAudioModule::process(const ProcessArgs &args) {
   if (!g_jack_client.alive()) return;

   // == PREPARE SAMPLE RATE STUFF ==
   int sampleRate = (int) args.sampleRate;
   inputSrc.setRates(g_jack_client.samplerate, sampleRate);
   outputSrc.setRates(sampleRate, g_jack_client.samplerate);

   // == FROM JACK TO RACK ==
   if (rack_input_buffer.empty() && !jack_input_buffer.empty()) {
      int inLen = jack_input_buffer.size();
      int outLen = rack_input_buffer.capacity();
      inputSrc.process
	 (jack_input_buffer.startData(),
	  &inLen, rack_input_buffer.endData(), &outLen);
      jack_input_buffer.startIncr(inLen);
      rack_input_buffer.endIncr(outLen);
   }

   if (!rack_input_buffer.empty()) {
      dsp::Frame<AUDIO_OUTPUTS> input_frame = rack_input_buffer.shift();
      for (int i = 0; i < AUDIO_INPUTS; i++) {
	 outputs[AUDIO_OUTPUT+i].setVoltage(input_frame.samples[i] * 10.0f);
      }
   }

   // == FROM RACK TO JACK ==
   if (!rack_output_buffer.full()) {
      dsp::Frame<AUDIO_OUTPUTS> outputFrame;
      for (int i = 0; i < AUDIO_OUTPUTS; i++) {
	 outputFrame.samples[i] = inputs[AUDIO_INPUT + i].getVoltage() / 10.0f;
      }
      rack_output_buffer.push(outputFrame);
   }

   if (rack_output_buffer.full()) {
      int inLen = rack_output_buffer.size();
      int outLen = jack_output_buffer.capacity();
      outputSrc.process
	 (rack_output_buffer.startData(),
	  &inLen, jack_output_buffer.endData(), &outLen);
      rack_output_buffer.startIncr(inLen);
      jack_output_buffer.endIncr(outLen);
   }

   // TODO: consider capping this? although an overflow here doesn't cause crashes...
   if (jack_output_buffer.size() > (g_jack_client.buffersize * 8)) {
      report_backlogged();
   }
}

void jack_audio_module_base::report_backlogged() {
   // we're over half capacity, so set our output latch
   if (output_latch.try_set()) {
      g_audio_blocked++;
   }

   // if everyone is output latched, stall Rack
   if (g_audio_blocked >= g_audio_modules.size()) {
      std::unique_lock<std::mutex> lock(jmutex);
      g_jack_cv.wait(lock);
   }
}

void jack_audio_module_base::assign_stupid_port_names() {
   // TODO deduplicate with same code on the widget side
   /* use the pointer to ourselves as a random unique port name */
   char port_name[128];
   if (g_jack_client.alive()) {
      hashidsxx::Hashids hash(g_hashid_salt);
      std::string id = hash.encode(reinterpret_cast<size_t>(this));

      for (int i = 0; i < JACK_PORTS; i++) {
	 snprintf
	    (reinterpret_cast<char*>(&port_name),
	     128,
	     "%s:%d",
	     id.c_str(),
	     i);

	 unsigned int flags = 0;
	 switch (role) {
	    case ROLE_DUPLEX:
	       flags = (i < AUDIO_OUTPUTS ? JackPortIsOutput : JackPortIsInput);
	       break;
	    case ROLE_OUTPUT:
	       flags = JackPortIsOutput;
	       break;
	    case ROLE_INPUT:
	       flags = JackPortIsInput;
	       break;
	 }

	 jport[i].register_audio
	    (g_jack_client,
	     reinterpret_cast<const char*>(&port_name), flags);
      }
   }
}

json_t* jack_audio_module_base::toJson() {
   auto map = Module::toJson();
   auto pt_names = json_array();

   for (int i = 0; i < JACK_PORTS; i++) {
      auto str = json_string(this->port_names[i].c_str());
      json_array_append_new(pt_names, str);
   }

   json_object_set_new(map, "port_names", pt_names);
   return map;
}

void jack_audio_module_base::fromJson(json_t* json) {
   auto module = reinterpret_cast<JackAudioModule*>(this);
   auto pt_names = json_object_get(json, "port_names");
   if (json_is_array(pt_names)) {
      for (size_t i = 0; i < std::min(json_array_size(pt_names), (size_t)8); i++) {
    auto item = json_array_get(pt_names, i);
    if (json_is_string(item)) {
       int error = module->jport[i].rename(json_string_value(item));
       if (error == 0) {
           DEBUG("Changing port name to %s successful",this->port_names[i].c_str());
           this->port_names[i] = std::string(json_string_value(item));
       } else {
           DEBUG("Changing port name failed: %s, %d", json_string_value(item), error);
           static const size_t buffer_size = 128;
           char port_name[buffer_size];
           hashidsxx::Hashids hash(g_hashid_salt);
           std::string id = hash.encode(reinterpret_cast<size_t>(module));

           snprintf(reinterpret_cast<char*>(&port_name),
             buffer_size,
             "%s:%d", id.c_str(), (int)i);
          this->port_names[i] = std::string(port_name);
       }
    }
      }
   }
}

JackAudioModule::JackAudioModule()
   : jack_audio_module_base(NUM_PARAMS, NUM_INPUTS, NUM_OUTPUTS, NUM_LIGHTS)
{
   assign_stupid_port_names();

   inputSrc.setChannels(AUDIO_INPUTS);
   outputSrc.setChannels(AUDIO_OUTPUTS);

   globally_register();
}

jack_audio_module_base::jack_audio_module_base
(size_t params, size_t inputs, size_t outputs, size_t lights)
   : Module(params, inputs, outputs, lights),
     role(ROLE_DUPLEX),
     output_latch(), inputSrc(), outputSrc()
{
}

jack_audio_module_base::~jack_audio_module_base() {
   // unregister from client
   globally_unregister();

   // kill our port
   if (!g_jack_client.alive()) return;
   for (int i = 0; i < JACK_PORTS; i++) {
      jport[i].unregister();
   }
}

void jack_audio_module_base::wipe_buffers() {
   rack_input_buffer.clear();
   rack_output_buffer.clear();
   jack_input_buffer.clear();
   jack_output_buffer.clear();
}

void jack_audio_module_base::globally_register() {
   std::unique_lock<std::mutex> lock(g_audio_modules_mutex);

   g_audio_modules.push_back(this);

   /* ensure modules are not filling up their buffers out of sync */
   for (auto itr = g_audio_modules.begin();
	itr != g_audio_modules.end();
	itr++)
   {
      (*itr)->wipe_buffers();
   }
}

void jack_audio_module_base::globally_unregister() {
   std::unique_lock<std::mutex> lock(g_audio_modules_mutex);

   /* drop ourselves from active module list */
   auto x = std::find(g_audio_modules.begin(), g_audio_modules.end(), this);
   if (x != g_audio_modules.end())
      g_audio_modules.erase(x);
}

JackAudioModule::~JackAudioModule() {}

jack_audio_out8_module::jack_audio_out8_module()
   : jack_audio_module_base(NUM_PARAMS, NUM_INPUTS, NUM_OUTPUTS, NUM_LIGHTS)
{
   role = ROLE_OUTPUT;
   assign_stupid_port_names();

   inputSrc.setChannels(AUDIO_INPUTS);
   outputSrc.setChannels(AUDIO_OUTPUTS);

   globally_register();
}

jack_audio_out8_module::~jack_audio_out8_module() {}

void jack_audio_out8_module::process(const ProcessArgs &args) {
   if (!g_jack_client.alive()) return;

   // == PREPARE SAMPLE RATE STUFF ==
   int sampleRate = (int) args.sampleRate;
   // not a bug; we're abusing both input and output pipes to be output pipes
   inputSrc.setRates(sampleRate, g_jack_client.samplerate);
   outputSrc.setRates(sampleRate, g_jack_client.samplerate);

   // == FROM RACK TO JACK ==
   if (!rack_output_buffer.full()) {
      dsp::Frame<AUDIO_OUTPUTS> outputFrame;
      for (int i = 0; i < 4; i++) {
	 outputFrame.samples[i] = inputs[AUDIO_INPUT + i].getVoltage() / 10.0f;
      }
      rack_output_buffer.push(outputFrame);

      for (int i = 0; i < 4; i++) {
	 outputFrame.samples[i] = inputs[AUDIO_INPUT + i + 4].getVoltage() / 10.0f;
      }
      rack_input_buffer.push(outputFrame);
   }

   if (rack_output_buffer.full()) {
      int inLen = rack_output_buffer.size();
      int outLen = jack_output_buffer.capacity();
      outputSrc.process
	 (rack_output_buffer.startData(),
	  &inLen, jack_output_buffer.endData(), &outLen);
      rack_output_buffer.startIncr(inLen);
      jack_output_buffer.endIncr(outLen);

      inLen = rack_input_buffer.size();
      outLen = jack_input_buffer.capacity();
      inputSrc.process
	 (rack_input_buffer.startData(),
	  &inLen, jack_input_buffer.endData(), &outLen);
      rack_input_buffer.startIncr(inLen);
      jack_input_buffer.endIncr(outLen);
   }

   // TODO: consider capping this?
   // although an overflow here doesn't cause crashes...
   if (jack_output_buffer.size() > (g_jack_client.buffersize * 8)) {
      report_backlogged();
   }
}

jack_audio_in8_module::jack_audio_in8_module()
   : jack_audio_module_base(NUM_PARAMS, NUM_INPUTS, NUM_OUTPUTS, NUM_LIGHTS)
{
   role = ROLE_INPUT;
   assign_stupid_port_names();

   inputSrc.setChannels(AUDIO_INPUTS);
   outputSrc.setChannels(AUDIO_OUTPUTS);

   globally_register();
}

jack_audio_in8_module::~jack_audio_in8_module() {}

void jack_audio_in8_module::process(const ProcessArgs &args) {
   if (!g_jack_client.alive()) return;

   // == PREPARE SAMPLE RATE STUFF ==
   int sampleRate = (int) args.sampleRate;
   // not a bug; we're abusing both input and output pipes to be output pipes
   inputSrc.setRates(g_jack_client.samplerate, sampleRate);
   outputSrc.setRates(g_jack_client.samplerate, sampleRate);

   // == FROM JACK TO RACK ==
   if (rack_output_buffer.empty() && !jack_output_buffer.empty()) {
      int inLen = jack_output_buffer.size();
      int outLen = rack_output_buffer.capacity();
      outputSrc.process
	 (jack_output_buffer.startData(),
	  &inLen, rack_output_buffer.endData(), &outLen);
      jack_output_buffer.startIncr(inLen);
      rack_output_buffer.endIncr(outLen);
   }

   if (!rack_output_buffer.empty()) {
      dsp::Frame<AUDIO_OUTPUTS> output_frame = rack_output_buffer.shift();
      for (int i = 0; i < AUDIO_OUTPUTS; i++) {
	 outputs[AUDIO_OUTPUT+i].setVoltage(output_frame.samples[i] * 10.0f);
      }
   }

   if (rack_input_buffer.empty() && !jack_input_buffer.empty()) {
      int inLen = jack_input_buffer.size();
      int outLen = rack_input_buffer.capacity();
      inputSrc.process
	 (jack_input_buffer.startData(),
	  &inLen, rack_input_buffer.endData(), &outLen);
      jack_input_buffer.startIncr(inLen);
      rack_input_buffer.endIncr(outLen);
   }

   if (!rack_input_buffer.empty()) {
      dsp::Frame<AUDIO_INPUTS> input_frame = rack_input_buffer.shift();
      for (int i = 0; i < AUDIO_INPUTS; i++) {
	 outputs[AUDIO_OUTPUT+AUDIO_OUTPUTS+i].value =
	   input_frame.samples[i] * 10.0f;
      }
   }

   if (jack_output_buffer.size() < (g_jack_client.buffersize * 8)) {
      report_backlogged();
   }
}
