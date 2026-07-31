# Final Reflection — EEL 4775 Real-Time Systems, Summer 2026

The biggest thing I would do differently is measuring worst-case execution time from the beginning instead of waiting until the capstone. App 3 gave us a WCET macro, but I treated it as a one 
assignment tool and did not touch it again until the final project. When I finally wrapped my App 5 tasks in it, the numbers changed how I saw my own system. My logging turned out to cost more than my actual work, and I realized I had been sizing queues and stall budgets by estimating things I could have just measured. I also learned to check that my data is valid before collecting it, not 
after. I only came up with my five press verification test after two bad measurement runs, which meant repeating everything. The giveaway was in the logs. Packets were printing at the same 
millisecond, which meant one button press was generating around ten wakes instead of one. The root cause was one missing "bounce":"0" line in my diagram.json file. I lost hours to a config file,
 not code. From now on, I will always compare my setup against a known working example before assuming the bug is in my logic.

The hardest part of this course was learning how concurrency really works. I expected bugs to happen because my code was too slow, but I had the opposite problem. In App 5, my consumer was too
fast. The scaffold's placeholder had been hiding a startup race, and once my real receive call ran at full speed, the pipeline finished a cycle before app_main had even created the responder task 
The program crashed with a NULL handle assertion and kept boot looping. Tasks on one core were outrunning the setup code on the other. The second hard lesson was trusting my measurements. My first 
latency test showed an average of about 37 microseconds, which looked fine, but the maximum was almost 40 times higher. At first I thought I could ignore it. Then I realized a max that far from the
 average meant the test itself was broken, and I had to throw the run away. Collecting good data takes patience. It is not enough for the code to work. You also need results you can explain and 
 defend.


The most valuable thing I learned is that being able to see what your system is doing matters as much as making it work. The heartbeat counters started as four variables feeding a web page. By the end, they were how I verified the pipeline was healthy, how I caught bad measurement runs, how I detected the fault I injected in my capstone demo, and a row in my hazard analysis. They became one 
of the most useful parts of my project. I also came to appreciate something the course said directly, which is that reusing your own work is engineering, not cheating. My App 1 web server became 
App 5's monitor, App 3's measurement patterns became my capstone's timing evidence, and citing that chain made the final system stronger. I used AI throughout the term to help me understand ideas 
and think through problems, but I tested everything myself. The code, the measurements, the debugging, and the final results came from my own work. If I take one habit into my next project, it is 
that every number needs a way to prove itself, and every system needs a way to show you what it is doing.

