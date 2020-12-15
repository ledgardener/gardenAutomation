# gardenAutomation

I'm new to github and writing code in general so please bear with me. This project is literally my first go at programming something and I'm having a blast. Experienced programmers may be scratching their heads at the way I've done some things but I'm learning and am always looking to improve. Over the past few weeks I've been trying to clean up my code and add as many useful comments as I can. Since I first posted my Home Assistant configuration files, I've made some significant changes to my MQTT topics and payloads that go back and forth between the Pi and the control box in order to make things more understandable and logical. I hope I've succeeded in my goal.

**I want to stress that I'm just a hobbyist and am not guaranteeing any of my electronics work or code. I'm no expert but am eager to share what I have, and while this system works for me, if you decide to make use of any of its components, it's up to you to vet yours and ensure everything is safe and compatible. You should always have a back-up plan to stop/drain water somehow in case of an overflow - I have an overflow drain line on each of my basins/reservoirs as well as float valves. I would suggest modifying the "Dose Nutrients" script in Home Assistant in order to suit your reservoir size and put a safety in place in the code to prevent it from overfilling the reservoir when diluting nutrients.** 

This Home Assistant project is now running on a Raspberry Pi 4 and is a mish-mash of many components. Here are its current capabilities:
- Measure temperature and humidity in control box and grow tent
- Measure water level, temperature, pH, and conductivity in nutrient mix reservoir
- Automatically dose pH up or down to maintain a target pH set in the UI
- Create a new batch of nutrients that hits a target EC based on inputs on UI
- Control grow light dimming (Mean Well B-type drivers via PWM)
- Control exhaust fan via PWM
- Detect flooding in tent or beneath control system via moisture sensors and shut off all pumps and solenoids when this happens
- Stir nutrients in glass jars to keep solution mixed, using magnetic stir solution
- Schedule light on/off times and customizable plant feed times/durations based on UI input
- Calibrate pH probe, EC probe, weigh scale (measures mixing reservoir volume in litres since 1 liter = 1 kilogram), and pump speed from UI

The Pi running Home Assistant is boss in this system. It has no physical I/O hooked up to it at all - it just sits on the network and communicates over MQTT with the ESP32 in the control box. You will need to install the MQTT Broker add-on in Home Assistant. The ESP32 has a lot of I/O hooked up to it, and it also communicates with the Arduino Mega via Serial, which handles some physical I/O and has the Tentacle shield on it that talks to the Atlas sensors. The ESP32 relays data from the Pi to the Mega and vice versa since the Mega has no network connection.

I've done my best to draw out the entire system down to conductor level, and have uploaded a diagram here as well. Don't even bother trying to view it in github's preview panel - open the .png up on your computer instead since it's huge. Once you open it, if you try to zoom in on something, you may need to be patient and let it load. The resolution is definitely there and if you give it time, when it renders, you'll be able to zoom in and see pin numbers nice and sharp. 

I've also included a parts list for everything in the system with links to all parts as well. 

To-do list:
- Wire up the pumps so they can be run in reverse as well as forward and program this in
- Incorporate VPD readings with some parts I've got waiting to be installed
- Change how the exhaust fan works (if the ESP PWM stops, the fan stops, so I'd like to safeguard against that, and I'd like to change how the fan operates based on humidity and temperature conditions) 
- Bring motion detection back with PIR to turn room lights on 
- Incorporate sunrise/sunset ramping of tent lights
