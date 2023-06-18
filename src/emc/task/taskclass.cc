/********************************************************************
* Description: iotaskintf.cc
*   NML interface functions for IO
*
*   Based on a work by Fred Proctor & Will Shackleford
*
* Author:
* License: GPL Version 2
* System: Linux
*
* Copyright (c) 2004 All rights reserved.
*
* Last change:
********************************************************************/

#include <math.h>		// fabs()
#include <float.h>		// DBL_MAX
#include <string.h>		// memcpy() strncpy()
#include <stdlib.h>		// malloc()
#include <sys/wait.h>

#include "rcs.hh"		// RCS_CMD_CHANNEL, etc.
#include "rcs_print.hh"
#include "timer.hh"             // esleep, etc.
#include "emc.hh"		// EMC NML
#include "emc_nml.hh"
#include "emcglb.h"		// EMC_INIFILE

#include "python_plugin.hh"
#include "taskclass.hh"
#include <rtapi_string.h>

Task *task_methods;

// IO INTERFACE

// the NML channels to the EMCIO controller
static RCS_CMD_CHANNEL *emcIoCommandBuffer = 0;
static RCS_STAT_CHANNEL *emcIoStatusBuffer = 0;

// global status structure
EMC_IO_STAT *emcIoStatus = 0;

// serial number for communication
static int emcIoCommandSerialNumber = 0;
static double EMCIO_BUFFER_GET_TIMEOUT = 5.0;

static int forceCommand(RCS_CMD_MSG *msg);

static int emcioNmlGet()
{
    int retval = 0;
    double start_time;
    RCS_PRINT_DESTINATION_TYPE orig_dest;
    if (emcIoCommandBuffer == 0) {
	orig_dest = get_rcs_print_destination();
	set_rcs_print_destination(RCS_PRINT_TO_NULL);
	start_time = etime();
	while (start_time - etime() < EMCIO_BUFFER_GET_TIMEOUT) {
	    emcIoCommandBuffer =
		new RCS_CMD_CHANNEL(emcFormat, "toolCmd", "emc",
				    emc_nmlfile);
	    if (!emcIoCommandBuffer->valid()) {
		delete emcIoCommandBuffer;
		emcIoCommandBuffer = 0;
	    } else {
		break;
	    }
	    esleep(0.1);
	}
	set_rcs_print_destination(orig_dest);
    }

    if (emcIoCommandBuffer == 0) {
	emcIoCommandBuffer =
	    new RCS_CMD_CHANNEL(emcFormat, "toolCmd", "emc", emc_nmlfile);
	if (!emcIoCommandBuffer->valid()) {
	    delete emcIoCommandBuffer;
	    emcIoCommandBuffer = 0;
	    retval = -1;
	}
    }

    if (emcIoStatusBuffer == 0) {
	orig_dest = get_rcs_print_destination();
	set_rcs_print_destination(RCS_PRINT_TO_NULL);
	start_time = etime();
	while (start_time - etime() < EMCIO_BUFFER_GET_TIMEOUT) {
	    emcIoStatusBuffer =
		new RCS_STAT_CHANNEL(emcFormat, "toolSts", "emc",
				     emc_nmlfile);
	    if (!emcIoStatusBuffer->valid()) {
		delete emcIoStatusBuffer;
		emcIoStatusBuffer = 0;
	    } else {
		emcIoStatus =
		    (EMC_IO_STAT *) emcIoStatusBuffer->get_address();
		// capture serial number for next send
		emcIoCommandSerialNumber = emcIoStatus->echo_serial_number;
		break;
	    }
	    esleep(0.1);
	}
	set_rcs_print_destination(orig_dest);
    }

    if (emcIoStatusBuffer == 0) {
	emcIoStatusBuffer =
	    new RCS_STAT_CHANNEL(emcFormat, "toolSts", "emc", emc_nmlfile);
	if (!emcIoStatusBuffer->valid()
	    || EMC_IO_STAT_TYPE != emcIoStatusBuffer->peek()) {
	    delete emcIoStatusBuffer;
	    emcIoStatusBuffer = 0;
	    emcIoStatus = 0;
	    retval = -1;
	} else {
	    emcIoStatus = (EMC_IO_STAT *) emcIoStatusBuffer->get_address();
	    // capture serial number for next send
	    emcIoCommandSerialNumber = emcIoStatus->echo_serial_number;
	}
    }

    return retval;
}

static RCS_CMD_MSG *last_io_command = 0;
static long largest_io_command_size = 0;

/*
  sendCommand() waits until any currently executing command has finished,
  then writes the given command.*/
/*! \todo
  FIXME: Not very RCS-like to wait for status done here. (wps)
*/
static int sendCommand(RCS_CMD_MSG * msg)
{
    // need command buffer to be there
    if (0 == emcIoCommandBuffer) {
	return -1;
    }
    // need status buffer also, to check for command received
    if (0 == emcIoStatusBuffer || !emcIoStatusBuffer->valid()) {
	return -1;
    }

    // always force-queue an abort
    if (msg->type == EMC_TOOL_ABORT_TYPE) {
	// just queue the abort and call it a day
	int rc = forceCommand(msg);
	if (rc) {
	    rcs_print_error("forceCommand(EMC_TOOL_ABORT) returned %d\n", rc);
	}
	return 0;
    }

    double send_command_timeout = etime() + 5.0;

    // check if we're executing, and wait until we're done
    while (etime() < send_command_timeout) {
	emcIoStatusBuffer->peek();
	if (emcIoStatus->echo_serial_number != emcIoCommandSerialNumber ||
	    emcIoStatus->status == RCS_STATUS::EXEC) {
	    esleep(0.001);
	    continue;
	} else {
	    break;
	}
    }

    if (emcIoStatus->echo_serial_number != emcIoCommandSerialNumber ||
	emcIoStatus->status == RCS_STATUS::EXEC) {
	// Still not done, must have timed out.
	rcs_print_error
	    ("Command to IO level (%s:%s) timed out waiting for last command done. \n",
	     emcSymbolLookup(msg->type), emcIoCommandBuffer->msg2str(msg));
	rcs_print_error
	    ("emcIoStatus->echo_serial_number=%d, emcIoCommandSerialNumber=%d, emcIoStatus->status=%d\n",
	     emcIoStatus->echo_serial_number, emcIoCommandSerialNumber,
	     emcIoStatus->status);
	if (0 != last_io_command) {
	    rcs_print_error("Last command sent to IO level was (%s:%s)\n",
			    emcSymbolLookup(last_io_command->type),
			    emcIoCommandBuffer->msg2str(last_io_command));
	}
	return -1;
    }
    // now we can send
    msg->serial_number = ++emcIoCommandSerialNumber;
    if (0 != emcIoCommandBuffer->write(msg)) {
	rcs_print_error("Failed to send command to  IO level (%s:%s)\n",
			emcSymbolLookup(msg->type),
			emcIoCommandBuffer->msg2str(msg));
	return -1;
    }

    if (largest_io_command_size < msg->size) {
	largest_io_command_size = std::max<long>(msg->size, 4096);
	last_io_command = (RCS_CMD_MSG *) realloc(last_io_command, largest_io_command_size);
    }

    if (0 != last_io_command) {
	memcpy(last_io_command, msg, msg->size);
    }

    return 0;
}

/*
  forceCommand() writes the given command regardless of the executing
  status of any previous command.
*/
static int forceCommand(RCS_CMD_MSG * msg)
{
    // need command buffer to be there
    if (0 == emcIoCommandBuffer) {
	return -1;
    }
    // need status buffer also, to check for command received
    if (0 == emcIoStatusBuffer || !emcIoStatusBuffer->valid()) {
	return -1;
    }
    // send it immediately
    msg->serial_number = ++emcIoCommandSerialNumber;
    if (0 != emcIoCommandBuffer->write(msg)) {
	rcs_print_error("Failed to send command to  IO level (%s:%s)\n",
			emcSymbolLookup(msg->type),
			emcIoCommandBuffer->msg2str(msg));
	return -1;
    }

    if (largest_io_command_size < msg->size) {
	largest_io_command_size = std::max<long>(msg->size, 4096);
	last_io_command = (RCS_CMD_MSG *) realloc(last_io_command, largest_io_command_size);
    }

    if (0 != last_io_command) {
	memcpy(last_io_command, msg, msg->size);
    }

    return 0;
}

// glue

int emcIoInit() { return task_methods->emcIoInit(); }

int emcIoHalt() {
	return task_methods->emcIoHalt();
}


int emcIoAbort(int reason) { return task_methods->emcIoAbort(reason); }
int emcIoSetDebug(int debug) { return task_methods->emcIoSetDebug(debug); }
int emcAuxEstopOn()  { return task_methods->emcAuxEstopOn(); }
int emcAuxEstopOff() { return task_methods->emcAuxEstopOff(); }
int emcCoolantMistOn() { return task_methods->emcCoolantMistOn(); }
int emcCoolantMistOff() { return task_methods->emcCoolantMistOff(); }
int emcCoolantFloodOn() { return task_methods->emcCoolantFloodOn(); }
int emcCoolantFloodOff() { return task_methods->emcCoolantFloodOff(); }
int emcLubeOn() { return task_methods->emcLubeOn(); }
int emcLubeOff() { return task_methods->emcLubeOff(); }
int emcToolPrepare(int tool) { return task_methods->emcToolPrepare(tool); }
int emcToolStartChange() { return task_methods->emcToolStartChange(); }
int emcToolLoad() { return task_methods->emcToolLoad(); }
int emcToolUnload()  { return task_methods->emcToolUnload(); }
int emcToolLoadToolTable(const char *file) { return task_methods->emcToolLoadToolTable(file); }
int emcToolSetOffset(int pocket, int toolno, EmcPose offset, double diameter,
                     double frontangle, double backangle, int orientation) {
    return task_methods->emcToolSetOffset( pocket,  toolno,  offset,  diameter,
					   frontangle,  backangle,  orientation); }
int emcToolSetNumber(int number) { return task_methods->emcToolSetNumber(number); }
int emcIoUpdate(EMC_IO_STAT * stat) { return task_methods->emcIoUpdate(stat); }
static const char *instance_name = "task_instance";

int emcTaskOnce(const char *filename)
{
	task_methods = new Task();
    return 0;
}


extern "C" PyObject* PyInit_interpreter(void);
extern "C" PyObject* PyInit_emccanon(void);
struct _inittab builtin_modules[] = {
    { "interpreter", PyInit_interpreter },
    { "emccanon", PyInit_emccanon },
    // any others...
    { NULL, NULL }
};

Task::Task() : random_toolchanger(0) {

    IniFile inifile;

    ini_filename = emc_inifile;

    if (inifile.Open(ini_filename)) {
	inifile.Find(&random_toolchanger, "RANDOM_TOOLCHANGER", "EMCIO");
	const char *t;
	if ((t = inifile.Find("TOOL_TABLE", "EMCIO")) != NULL)
	    tooltable_filename = strdup(t);
    }
};


Task::~Task() {};

// set the have_tool_change_position global
static int readToolChange(IniFile *toolInifile)
{
    int retval = 0;
    const char *inistring;

    if (NULL !=
	(inistring = toolInifile->Find("TOOL_CHANGE_POSITION", "EMCIO"))) {
	/* found an entry */
        if (9 == sscanf(inistring, "%lf %lf %lf %lf %lf %lf %lf %lf %lf",
                        &tool_change_position.tran.x,
                        &tool_change_position.tran.y,
                        &tool_change_position.tran.z,
                        &tool_change_position.a,
                        &tool_change_position.b,
                        &tool_change_position.c,
                        &tool_change_position.u,
                        &tool_change_position.v,
                        &tool_change_position.w)) {
            have_tool_change_position=9;
            retval=0;
        } else if (6 == sscanf(inistring, "%lf %lf %lf %lf %lf %lf",
                        &tool_change_position.tran.x,
                        &tool_change_position.tran.y,
                        &tool_change_position.tran.z,
                        &tool_change_position.a,
                        &tool_change_position.b,
                        &tool_change_position.c)) {
	    tool_change_position.u = 0.0;
	    tool_change_position.v = 0.0;
	    tool_change_position.w = 0.0;
            have_tool_change_position = 6;
            retval = 0;
        } else if (3 == sscanf(inistring, "%lf %lf %lf",
                               &tool_change_position.tran.x,
                               &tool_change_position.tran.y,
                               &tool_change_position.tran.z)) {
	    /* read them OK */
	    tool_change_position.a = 0.0;
	    tool_change_position.b = 0.0;
	    tool_change_position.c = 0.0;
	    tool_change_position.u = 0.0;
	    tool_change_position.v = 0.0;
	    tool_change_position.w = 0.0;
	    have_tool_change_position = 3;
	    retval = 0;
	} else {
	    /* bad format */
	    rcs_print("bad format for TOOL_CHANGE_POSITION\n");
	    have_tool_change_position = 0;
	    retval = -1;
	}
    } else {
	/* didn't find an entry */
	have_tool_change_position = 0;
    }
    return retval;
}

static int iniTool(const char *filename)
{
    int retval = 0;
    IniFile toolInifile;

    if (toolInifile.Open(filename) == false) {
	return -1;
    }
    // read the tool change positions
    if (0 != readToolChange(&toolInifile)) {
	retval = -1;
    }
    // close the inifile
    toolInifile.Close();

    return retval;
}

// NML commands

int Task::emcIoInit()
{
    EMC_TOOL_INIT ioInitMsg;

    // get NML buffer to emcio
    if (0 != emcioNmlGet()) {
	rcs_print_error("emcioNmlGet() failed.\n");
	return -1;
    }

    if (0 != iniTool(emc_inifile)) {
	return -1;
    }

    // send init command to emcio
    if (forceCommand(&ioInitMsg)) {
	rcs_print_error("Can't forceCommand(ioInitMsg)\n");
	return -1;
    }

    return 0;
}

int Task::emcIoHalt()
{
    EMC_TOOL_HALT ioHaltMsg;

    // send halt command to emcio
    if (emcIoCommandBuffer != 0) {
	forceCommand(&ioHaltMsg);
    }
    // clear out the buffers

    if (emcIoStatusBuffer != 0) {
	delete emcIoStatusBuffer;
	emcIoStatusBuffer = 0;
	emcIoStatus = 0;
    }

    if (emcIoCommandBuffer != 0) {
	delete emcIoCommandBuffer;
	emcIoCommandBuffer = 0;
    }

    if (last_io_command) {
        free(last_io_command);
        last_io_command = 0;
    }

    return 0;
}

int Task::emcIoAbort(int reason)
{
    EMC_TOOL_ABORT ioAbortMsg;

    ioAbortMsg.reason = reason;
    // send abort command to emcio
    sendCommand(&ioAbortMsg);

    return 0;
}

int Task::emcIoSetDebug(int debug)
{
    EMC_SET_DEBUG ioDebugMsg;

    ioDebugMsg.debug = debug;

    return sendCommand(&ioDebugMsg);
}

int Task::emcAuxEstopOn()
{
    EMC_AUX_ESTOP_ON estopOnMsg;

    return forceCommand(&estopOnMsg);
}

int Task::emcAuxEstopOff()
{
    EMC_AUX_ESTOP_OFF estopOffMsg;

    return forceCommand(&estopOffMsg); //force the EstopOff message
}

int Task::emcCoolantMistOn()
{
    EMC_COOLANT_MIST_ON mistOnMsg;

    sendCommand(&mistOnMsg);

    return 0;
}

int Task::emcCoolantMistOff()
{
    EMC_COOLANT_MIST_OFF mistOffMsg;

    sendCommand(&mistOffMsg);

    return 0;
}

int Task::emcCoolantFloodOn()
{
    EMC_COOLANT_FLOOD_ON floodOnMsg;

    sendCommand(&floodOnMsg);

    return 0;
}

int Task::emcCoolantFloodOff()
{
    EMC_COOLANT_FLOOD_OFF floodOffMsg;

    sendCommand(&floodOffMsg);

    return 0;
}

int Task::emcLubeOn()
{
    EMC_LUBE_ON lubeOnMsg;

    sendCommand(&lubeOnMsg);

    return 0;
}

int Task::emcLubeOff()
{
    EMC_LUBE_OFF lubeOffMsg;

    sendCommand(&lubeOffMsg);

    return 0;
}

int Task::emcToolPrepare(int tool)
{
    EMC_TOOL_PREPARE toolPrepareMsg;

    toolPrepareMsg.tool = tool;
    sendCommand(&toolPrepareMsg);

    return 0;
}


int Task::emcToolStartChange()
{
    EMC_TOOL_START_CHANGE toolStartChangeMsg;

    sendCommand(&toolStartChangeMsg);

    return 0;
}


int Task::emcToolLoad()
{
    EMC_TOOL_LOAD toolLoadMsg;

    sendCommand(&toolLoadMsg);

    return 0;
}

int Task::emcToolUnload()
{
    EMC_TOOL_UNLOAD toolUnloadMsg;

    sendCommand(&toolUnloadMsg);

    return 0;
}

int Task::emcToolLoadToolTable(const char *file)
{
    EMC_TOOL_LOAD_TOOL_TABLE toolLoadToolTableMsg;

    rtapi_strxcpy(toolLoadToolTableMsg.file, file);

    sendCommand(&toolLoadToolTableMsg);

    return 0;
}

int Task::emcToolSetOffset(int pocket, int toolno, EmcPose offset, double diameter,
                     double frontangle, double backangle, int orientation)
{
    EMC_TOOL_SET_OFFSET toolSetOffsetMsg;

    toolSetOffsetMsg.pocket = pocket;
    toolSetOffsetMsg.toolno = toolno;
    toolSetOffsetMsg.offset = offset;
    toolSetOffsetMsg.diameter = diameter;
    toolSetOffsetMsg.frontangle = frontangle;
    toolSetOffsetMsg.backangle = backangle;
    toolSetOffsetMsg.orientation = orientation;

    sendCommand(&toolSetOffsetMsg);

    return 0;
}

int Task::emcToolSetNumber(int number)
{
    EMC_TOOL_SET_NUMBER toolSetNumberMsg;

    toolSetNumberMsg.tool = number;

    sendCommand(&toolSetNumberMsg);

    return 0;
}

// Status functions

int Task::emcIoUpdate(EMC_IO_STAT * stat)
{
    if (0 == emcIoStatusBuffer || !emcIoStatusBuffer->valid()) {
	return -1;
    }

    switch (emcIoStatusBuffer->peek()) {
    case -1:
	// error on CMS channel
	return -1;
	break;

    case 0:			// nothing new
    case EMC_IO_STAT_TYPE:	// something new
	// drop out to copy
	break;

    default:
	// something else is in there
	return -1;
	break;
    }

    // copy status
    *stat = *emcIoStatus;

    /*
       We need to check that the RCS_DONE isn't left over from the previous
       command, by comparing the command number we sent with the command
       number that emcio echoes. If they're different, then the command
       hasn't been acknowledged yet and the state should be forced to be
       RCS_STATUS::EXEC. */
    if (stat->echo_serial_number != emcIoCommandSerialNumber) {
	stat->status = RCS_STATUS::EXEC;
    }
    //commented out because it keeps resetting the spindle speed to some odd value
    //the speed gets set by the IO controller, no need to override it here (io takes care of increase/decrease speed too)
    // stat->spindle.speed = spindleSpeed;

    return 0;
}
