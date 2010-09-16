%% This file compiles the simulation file for use on a dsPIC
%% microcontroller.

% This compilation flow follows the following steps:
%  1) Copy the controller from Boat_sim.mdl into existing codegen.mdl
%  replacing the old version there.
%  2) Builds the model with the RTW Embedded Coder to generate a .hex file
load_system('Boat_sim');
load_system('code_gen');
replace_block('code_gen', 'Name', 'Autonomous Controller', 'boat_sim/Autonomous Controller', 'noprompt');
save_system('code_gen');
save_system('boat_sim');
close_system('code_gen');
close_system('boat_sim');
rtwbuild('code_gen');