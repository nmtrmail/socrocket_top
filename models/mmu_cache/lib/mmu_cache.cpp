//*********************************************************************
// Copyright 2010, Institute of Computer and Network Engineering,
//                 TU-Braunschweig
// All rights reserved
// Any reproduction, use, distribution or disclosure of this program,
// without the express, prior written consent of the authors is 
// strictly prohibited.
//
// University of Technology Braunschweig
// Institute of Computer and Network Engineering
// Hans-Sommer-Str. 66
// 38118 Braunschweig, Germany
//
// ESA SPECIAL LICENSE
//
// This program may be freely used, copied, modified, and redistributed
// by the European Space Agency for the Agency's own requirements.
//
// The program is provided "as is", ther is no warranty that
// the program is correct or suitable for any purpose,
// neither implicit nor explicit. The program and the information in it
// contained do not necessarily reflect the policy of the 
// European Space Agency or of TU-Braunschweig.
//*********************************************************************
// Title:      mmu_cache.cpp
//
// ScssId:
//
// Origin:     HW-SW SystemC Co-Simulation SoC Validation Platform
//
// Purpose:    Class definition of a cache-subsystem.
//             The cache-subsystem envelopes an instruction cache,
//             a data cache and a memory management unit.
//             The mmu_cache class provides two TLM slave interfaces
//             for connecting the cpu to the caches and an AHB master
//             interface for connection to the main memory.
//
// Method:
//
// Modified on $Date$
//          at $Revision$
//          by $Author$
//
// Principal:  European Space Agency
// Author:     VLSI working group @ IDA @ TUBS
// Maintainer: Thomas Schuster
// Reviewed:
//*********************************************************************

#include "mmu_cache.h"
#include "vendian.h"

//SC_HAS_PROCESS(mmu_cache<>);
/// Constructor
mmu_cache::mmu_cache(unsigned int icen, unsigned int irepl, unsigned int isets,
                     unsigned int ilinesize, unsigned int isetsize,
                     unsigned int isetlock, unsigned int dcen,
                     unsigned int drepl, unsigned int dsets,
                     unsigned int dlinesize, unsigned int dsetsize,
                     unsigned int dsetlock, unsigned int dsnoop,
                     unsigned int ilram, unsigned int ilramsize,
                     unsigned int ilramstart, unsigned int dlram,
                     unsigned int dlramsize, unsigned int dlramstart,
                     unsigned int cached, unsigned int mmu_en,
                     unsigned int itlb_num, unsigned int dtlb_num,
                     unsigned int tlb_type, unsigned int tlb_rep,
                     unsigned int mmupgsz, sc_core::sc_module_name name,
                     unsigned int id,
		     bool pow_mon,
		     amba::amba_layer_ids abstractionLayer) :

    sc_module(name),
    AHBDevice(id,
	      0x01,  // vendor: Gaisler Research (Fake the LEON)
	      0x003,  // 
	      0,
	      0,
	      0,
	      0,
	      0,
	      0),
    icio("icio"), 
    dcio("dcio"), 
    ahb("ahb_socket", amba::amba_AHB, abstractionLayer, false),
    snoop(&mmu_cache::snoopingCallBack,"SNOOP"),
    icio_PEQ("icio_PEQ"), 
    dcio_PEQ("dcio_PEQ"),
    m_icen(icen), 
    m_dcen(dcen),
    m_dsnoop(dsnoop),
    m_ilram(ilram), 
    m_ilramstart(ilramstart),
    m_dlram(dlram),
    m_dlramstart(dlramstart),
    m_cached(cached),
    m_mmu_en(mmu_en),
    m_master_id(id), 
    m_pow_mon(pow_mon),
    m_abstractionLayer(abstractionLayer), 
    m_txn_count(0), 
    m_data_count(0),
    m_bus_granted(false), 
    current_trans(NULL),
    mResponsePEQ("ResponsePEQ"),
    mDataPEQ("DataPEQ"),
    mEndTransactionPEQ("EndTransactionPEQ"),
    clockcycle(10, sc_core::SC_NS) {

    // parameter checks
    // ----------------

    // check range of cacheability mask (0x0 - 0xffff)
    assert((m_cached>=0)&&(m_cached<=0xffff));

    // create mmu (if required)
    m_mmu = (mmu_en == 1)? new mmu("mmu", 
				   (mmu_cache_if *)this,
				   itlb_num,
				   dtlb_num,
				   tlb_type,
				   tlb_rep,
				   mmupgsz) : NULL;

    // create icache
    icache = (icen == 1)? (cache_if*)new ivectorcache("ivectorcache",
            (mmu_cache_if *)this, (mmu_en)? (mem_if *)m_mmu->get_itlb_if()
                                           : (mem_if *)this, mmu_en,
            isets, isetsize, isetlock, ilinesize, irepl, ilram, ilramstart,
            ilramsize, m_pow_mon) : (cache_if*)new nocache("no_icache",
            (mmu_en)? (mem_if *)m_mmu->get_itlb_if() : (mem_if *)this);

    // create dcache
    dcache = (dcen == 1)? (cache_if*)new dvectorcache("dvectorcache",
            (mmu_cache_if *)this, (mmu_en)? (mem_if *)m_mmu->get_dtlb_if()
                                           : (mem_if *)this, mmu_en,
            dsets, dsetsize, dsetlock, dlinesize, drepl, dlram, dlramstart, 
	    dlramsize, m_pow_mon) : (cache_if*)new nocache("no_dcache", 
	    (mmu_en)? (mem_if *)m_mmu->get_dtlb_if() : (mem_if *)this);

    // Create instruction scratchpad
    // (! only allowed with mmu disabled !)
    ilocalram = ((ilram == 1) && (mmu_en == 0))? new localram("ilocalram",
            ilramsize, ilramstart) : NULL;

    // Create data scratchpad
    // (! only allowed with mmu disabled !)
    dlocalram = ((dlram == 1) && (mmu_en == 0))? new localram("dlocalram",
            dlramsize, dlramstart) : NULL;

    // Loosely-Timed
    if (abstractionLayer==amba::amba_LT) {

      // Register blocking forward transport functions for icio and dcio sockets (slave)
      icio.register_b_transport(this, &mmu_cache::icio_b_transport);
      dcio.register_b_transport(this, &mmu_cache::dcio_b_transport);

    // Approximately-Timed
    } else if (abstractionLayer==amba::amba_AT) {

      // Register non-blocking forward transport functions for icio and dcio sockets
      icio.register_nb_transport_fw(this, &mmu_cache::icio_nb_transport_fw);
      dcio.register_nb_transport_fw(this, &mmu_cache::dcio_nb_transport_fw);

      // Register non-blocking backward transport function for ahb socket
      ahb.register_nb_transport_bw(this, &mmu_cache::ahb_nb_transport_bw);

      // Register icio service thread (for AT)
      SC_THREAD(icio_service_thread);
      sensitive << icio_PEQ.get_event();
      dont_initialize();

      // Register dcio service thread (for AT)
      SC_THREAD(dcio_service_thread);
      sensitive << dcio_PEQ.get_event();
      dont_initialize();

      SC_THREAD(ResponseThread);

      SC_THREAD(DataThread);

      // Delayed transaction release (for AT)
      SC_THREAD(cleanUP);

    } else {

      v::error << this->name() << "Abstraction Layer not valid!!" << v::endl;
      assert(0);

    }

    // Instruction debug transport
    icio.register_transport_dbg(this, &mmu_cache::icio_transport_dbg);
    // Data debug transport
    dcio.register_transport_dbg(this, &mmu_cache::dcio_transport_dbg);

    // Register power monitor
    PM::registerIP(this,"mmu_cache",m_pow_mon);
    PM::send_idle(this,"idle",sc_time_stamp(),m_pow_mon);

    // Initialize cache control registers
    CACHE_CONTROL_REG = 0;

}

// Instruction interface to functional part of the model
void mmu_cache::exec_instr(tlm::tlm_generic_payload& trans, sc_core::sc_time& delay, bool is_dbg) {

  // Vars for payload decoding
  tlm::tlm_command cmd = trans.get_command();
  sc_dt::uint64 addr   = trans.get_address();
  unsigned char * ptr  = trans.get_data_ptr();

  // Extract extension
  icio_payload_extension * iext;
  trans.get_extension(iext);

  unsigned int * debug = iext->debug;

  if (cmd == tlm::TLM_READ_COMMAND) {

    // Instruction scratchpad enabled && address points into selecte 16MB region
    if (m_ilram && (((addr >> 24) & 0xff) == m_ilramstart)) {

      ilocalram->mem_read((unsigned int)addr, ptr, 4, &delay, debug, is_dbg);

    // Instruction cache access
    } else {

      icache->mem_read((unsigned int)addr, ptr, 4, &delay, debug, is_dbg);
    
    }

    // Set response status
    trans.set_response_status(tlm::TLM_OK_RESPONSE);

  } else {

    v::error << name() << " Command not valid for instruction cache (tlm_write)" << v::endl;
    // Set response status
    trans.set_response_status(tlm::TLM_COMMAND_ERROR_RESPONSE);
    
  }
}

// Data interface to functional part of the model
void mmu_cache::exec_data(tlm::tlm_generic_payload& trans, sc_core::sc_time& delay, bool is_dbg) {

  // Vars for payload decoding
  tlm::tlm_command cmd = trans.get_command();
  sc_dt::uint64 addr   = trans.get_address();
  unsigned char * ptr  = trans.get_data_ptr();
  unsigned int len     = trans.get_data_length();

  // Extract extension
  dcio_payload_extension * dext;
  trans.get_extension(dext);

  unsigned int asi     = dext->asi;
  unsigned int * debug = dext->debug;

  // Access to system registers
  if (asi == 2) {

    // Reading system registers (ASI 0x2)
    if (cmd == tlm::TLM_READ_COMMAND) {

      v::debug << name() << "System Registers read with ASI 0x2 - addr:" << hex << addr << v::endl;

      // Address decoder for system registers
      if (addr == 0) {

	// Cache Control Register
        *(unsigned int *)ptr = read_ccr();
	// Setting response status
	trans.set_response_status(tlm::TLM_OK_RESPONSE);

      } else if (addr == 8) {

        // Instruction Cache Configuration Register
        *(unsigned int *)ptr = icache->read_config_reg(&delay);
        // Setting response status
        trans.set_response_status(tlm::TLM_OK_RESPONSE);

      } else if (addr == 0x0c) {

        // Data Cache Configuration Register
        *(unsigned int *)ptr = dcache->read_config_reg(&delay);
	// Setting response status
	trans.set_response_status(tlm::TLM_OK_RESPONSE);

      } else {

        v::error << name() << "Address not valid for read with ASI 0x2" << v::endl;
	// Set TLM response
	trans.set_response_status(tlm::TLM_ADDRESS_ERROR_RESPONSE);
      }

    // Writing system registers (ASI 0x2)
    } else if (cmd == tlm::TLM_WRITE_COMMAND) {

      v::debug << name() << "System Register write with ASI 0x2 - addr:" << hex << addr << v::endl;

      // Address decoder for system registers
      if (addr == 0) {

        // Cache Control Register
	write_ccr(ptr, len, &delay, is_dbg);
	// Setting response status
	trans.set_response_status(tlm::TLM_OK_RESPONSE);

      // TRIGGER ICACHE DEBUG OUTPUT / NOT A SPARC SYSTEM REGISTER
      } else if (addr == 0xfe) {

        // icache debug output (arg: line)
        icache->dbg_out(*(unsigned int*)ptr);
	// Setting response status
	trans.set_response_status(tlm::TLM_OK_RESPONSE);

      // TRIGGER DCACHE DEBUG OUTPUT / NOT A SPARC SYSTEM REGISTER
      } else if (addr == 0xff) {

        // dcache debug output (arg: line)
        dcache->dbg_out(*(unsigned int*)ptr);
	// Setting response status
	trans.set_response_status(tlm::TLM_OK_RESPONSE);

      } else {

        v::error << name() << "Address not valid for write with ASI 0x2 (or read only)" << v::endl;

	// Set TLM response
	trans.set_response_status(tlm::TLM_ADDRESS_ERROR_RESPONSE);

      }

    } else {

      v::error << name() << "Unvalid TLM Command" << v::endl;
      // Set TLM response
      trans.set_response_status(tlm::TLM_COMMAND_ERROR_RESPONSE);
  
    }

  // Diagnostic access to instruction PDC
  } else if (asi == 0x5) {

    // Reading instruction PDC (ASI 0x5)
    if (cmd == tlm::TLM_READ_COMMAND) {

      v::debug << name() << "Diagnostic read from instruction PDC (ASI 0x5)" << v::endl;

      // Only possible if mmu enabled
      if (m_mmu_en) {

        m_mmu->diag_read_itlb(addr, (unsigned int *)ptr);
	// Set TLM response
	trans.set_response_status(tlm::TLM_OK_RESPONSE);

      } else {

	v::error << name() << "MMU not present" << v::endl;
        // Set TLM response
        trans.set_response_status(tlm::TLM_ADDRESS_ERROR_RESPONSE);

      }

    // Writing instruction PDC (ASI 0x5)
    } else if (cmd == tlm::TLM_WRITE_COMMAND) {

       v::debug << name() << "Diagnostic write to instruction PDC (ASI 0x5)" << v::endl;

       // Only possible if mmu enabled
       if (m_mmu_en) {

          m_mmu->diag_write_itlb(addr, (unsigned int *)ptr);
	  // set TLM response
	  trans.set_response_status(tlm::TLM_OK_RESPONSE);

       } else {

	 v::error << name() << "MMU not present" << v::endl;
         // Set TLM response
         trans.set_response_status(tlm::TLM_ADDRESS_ERROR_RESPONSE);

       }

    } else {

      v::error << name() << "Unvalid TLM Command" << v::endl;
      // Set TLM response
      trans.set_response_status(tlm::TLM_COMMAND_ERROR_RESPONSE);

    }

  // Diagnostic access to data PDC or shared instruction and data PDC
  } else if (asi == 0x6) {

    // Reading instruction PDC (ASI 0x6)
    if (cmd == tlm::TLM_READ_COMMAND) {

      v::debug << name() << "Diagnostic read from data (or shared) PDC (ASI 0x6)" << v::endl;

      // Only possible if mmu enabled
      if (m_mmu_en) {

        m_mmu->diag_read_dctlb(addr, (unsigned int *)ptr);
	// Set TLM response
	trans.set_response_status(tlm::TLM_OK_RESPONSE);

      } else {

	v::error << name() << "MMU not present" << v::endl;

	// Set TLM response
	trans.set_response_status(tlm::TLM_ADDRESS_ERROR_RESPONSE);

      }

    // Writing instruction PDC (ASI 0x6)
    } else if (cmd == tlm::TLM_WRITE_COMMAND) {

      v::debug << name() << "Diagnostic write to data (or shared) PDC (ASI 0x6)" << v::endl;

      // Only possible if mmu enabled
      if (m_mmu_en) {

        m_mmu->diag_write_dctlb(addr, (unsigned int *)ptr);
	// Set TLM response
	trans.set_response_status(tlm::TLM_OK_RESPONSE);

      } else {

	v::error << name() << "MMU not present" << v::endl;
        // Set TLM response
        trans.set_response_status(tlm::TLM_ADDRESS_ERROR_RESPONSE);

      }

    } else {

      v::error << name() << "Unvalid TLM Command" << v::endl;
      // Set TLM response
      trans.set_response_status(tlm::TLM_COMMAND_ERROR_RESPONSE);

    }
  
  // Access instruction cache tags
  } else if (asi == 0xc) {

    // Reading instruction cache tags (ASI 0xc)
    if (cmd == tlm::TLM_READ_COMMAND) {

      v::debug << name() << "ASI read instruction cache tags" << v::endl;

      icache->read_cache_tag((unsigned int)addr, (unsigned int*)ptr, &delay);
      // Set TLM response
      trans.set_response_status(tlm::TLM_OK_RESPONSE);

    // Writing instruction cache tags (ASI 0xc)
    } else if (cmd == tlm::TLM_WRITE_COMMAND) {

      v::debug << name() << "ASI write instruction cache tags" << v::endl;

      icache->write_cache_tag((unsigned int)addr, (unsigned int*)ptr, &delay);
      // Set TLM response
      trans.set_response_status(tlm::TLM_OK_RESPONSE);

    } else {

      v::error << name() << "Unvalid TLM Command" << v::endl;
      // Set TLM response
      trans.set_response_status(tlm::TLM_COMMAND_ERROR_RESPONSE);

    }
  
  // Access instruction cache entries
  } else if (asi == 0xd) {

    // Reading instruction cache entries (ASI 0xd)
    if (cmd == tlm::TLM_READ_COMMAND) {

      v::debug << name() << "ASI read instruction cache entry" << v::endl;

      icache->read_cache_entry((unsigned int)addr, (unsigned int*)ptr, &delay);
      // Set TLM response
      trans.set_response_status(tlm::TLM_OK_RESPONSE);

    // Writing instruction cache entries (ASI 0xd)
    } else if (cmd == tlm::TLM_WRITE_COMMAND) {

      v::debug << name() << "ASI write instruction cache entry" << v::endl;

      icache->write_cache_entry((unsigned int)addr, (unsigned int*)ptr, &delay);

      // Set TLM response
      trans.set_response_status(tlm::TLM_OK_RESPONSE);

    } else {

      v::error << name() << "Unvalid TLM Command" << v::endl;
      // Set TLM response
      trans.set_response_status(tlm::TLM_COMMAND_ERROR_RESPONSE);

    }

  // Access data cache tags
  } else if (asi == 0xe) {

    // Reading data cache tags (ASI 0xe)
    if (cmd == tlm::TLM_READ_COMMAND) {

      v::debug << name() << "ASI read data cache tags" << v::endl;

      dcache->read_cache_tag((unsigned int)addr, (unsigned int*)ptr, &delay);
      // Set TLM response
      trans.set_response_status(tlm::TLM_OK_RESPONSE);

    // Writing data cache tags (ASI 0xe)
    } else if (cmd == tlm::TLM_WRITE_COMMAND) {

      v::debug << name() << "ASI write data cache tags" << v::endl;

      dcache->write_cache_tag((unsigned int)addr, (unsigned int*)ptr, &delay);
      // Set TLM response
      trans.set_response_status(tlm::TLM_OK_RESPONSE);

    } else {

      v::error << name() << "Unvalid TLM Command" << v::endl;
      // Set TLM response
      trans.set_response_status(tlm::TLM_COMMAND_ERROR_RESPONSE);

    }
    
  // Access data cache entries
  } else if (asi == 0xf) {

    // Reading data cache data (ASI 0xf)
    if (cmd == tlm::TLM_READ_COMMAND) {

      v::debug << name() << "ASI read data cache entry" << v::endl;

      dcache->read_cache_entry((unsigned int)addr, (unsigned int*)ptr, &delay);
      // Set TLM response
      trans.set_response_status(tlm::TLM_OK_RESPONSE);

    // Writing data cache entries (ASI 0xf)
    } else if (cmd == tlm::TLM_WRITE_COMMAND) {

      v::debug << name() << "ASI write data cache entry" << v::endl;

      dcache->write_cache_entry((unsigned int)addr, (unsigned int*)ptr, &delay);
      // Set TLM response
      trans.set_response_status(tlm::TLM_OK_RESPONSE);

    } else {

      v::error << name() << "Unvalid TLM Command" << v::endl;
      // Set TLM response
      trans.set_response_status(tlm::TLM_COMMAND_ERROR_RESPONSE);

    }

  // Flush instruction cache
  } else if (asi == 0x10) {

    // icache is flushed on any write with ASI 0x10
    if (cmd == tlm::TLM_WRITE_COMMAND) {

      v::debug << name() << "ASI flush instruction cache" << v::endl;

      icache->flush(&delay, debug, is_dbg);
      // Set TLM response
      trans.set_response_status(tlm::TLM_OK_RESPONSE);

    } else {

      v::error << name() << "Unvalid TLM Command" << v::endl;
      // Set TLM response
      trans.set_response_status(tlm::TLM_COMMAND_ERROR_RESPONSE);

    }
    
  // Flush data cache
  } else if (asi == 0x11) {

    // dcache is flushed on any write with ASI 0x11
    if (cmd == tlm::TLM_WRITE_COMMAND) {

       v::debug << name() << "ASI flush data cache" << v::endl;

       dcache->flush(&delay, debug, is_dbg);
       // Set TLM response
       trans.set_response_status(tlm::TLM_OK_RESPONSE);

    } else {

      v::error << name() << "Unvalid TLM Command" << v::endl;
      // Set TLM response
      trans.set_response_status(tlm::TLM_COMMAND_ERROR_RESPONSE);

    }
    
  // Access MMU internal registers
  } else if (asi == 0x19) {

    // Only works if MMU present
    if (m_mmu_en == 0x1) {

      // Read MMU internal registers
      if (cmd == tlm::TLM_READ_COMMAND) {

        v::debug << name() << "MMU register read with ASI 0x19 - addr: " << hex << addr << v::endl;

	// Address decoder for MMU register access
	if (addr == 0x000) {

          // MMU Control Register
          v::debug << name() << "ASI read MMU Control Register" << v::endl;

          *(unsigned int *)ptr = m_mmu->read_mcr();
	  // Set TLM response
	  trans.set_response_status(tlm::TLM_OK_RESPONSE);
		
        } else if (addr == 0x100) {

          // Context Pointer Register
          v::debug << name() << "ASI read MMU Context Pointer Register" << v::endl;

          *(unsigned int *)ptr = m_mmu->read_mctpr();
	  // Set TLM response
	  trans.set_response_status(tlm::TLM_OK_RESPONSE);

        } else if (addr == 0x200) {

          // Context Register
          v::debug << name() << "ASI read MMU Context Register" << v::endl;

          *(unsigned int *)ptr = m_mmu->read_mctxr();
	  // Set TLM response
	  trans.set_response_status(tlm::TLM_OK_RESPONSE);

        } else if (addr == 0x300) {

          // Fault Status Register
          v::debug << name() << "ASI read MMU Fault Status Register" << v::endl;

	  *(unsigned int *)ptr = m_mmu->read_mfsr();
	  // Set TLM response
	  trans.set_response_status(tlm::TLM_OK_RESPONSE);

        } else if (addr == 0x400) {

          // Fault Address Register
          v::debug << name() << "ASI read MMU Fault Address Register" << v::endl;

          *(unsigned int *)ptr = m_mmu->read_mfar();
	  // Set TLM response
	  trans.set_response_status(tlm::TLM_OK_RESPONSE);

        } else {
                    
	  v::error << name() << "Address not valid for read with ASI 0x19" << v::endl;

	  // Setting TLM response
	  trans.set_response_status(tlm::TLM_ADDRESS_ERROR_RESPONSE);

        }

      // Write MMU internal registers
      } else if (cmd == tlm::TLM_WRITE_COMMAND) {

        v::debug << name() << "MMU register write with ASI 0x19 - addr: " << hex << addr << v::endl;

	// Address decoder for MMU register access
        if (addr == 0x000) {

	  // MMU Control Register
          v::debug << name() << "ASI write MMU Control Register" << v::endl;

          m_mmu->write_mcr((unsigned int *)ptr);
	  // Set TLM response
	  trans.set_response_status(tlm::TLM_OK_RESPONSE);

        } else if (addr == 0x100) {

          // Context Table Pointer Register
          v::debug << name() << "ASI write MMU Context Table Pointer Register" << v::endl;

          m_mmu->write_mctpr((unsigned int*)ptr);
	  // Set TLM response
	  trans.set_response_status(tlm::TLM_OK_RESPONSE);

        } else if (addr == 0x200) {

	  // Context Register
          v::debug << name() << "ASI write MMU Context Register" << v::endl;

	  m_mmu->write_mctxr((unsigned int*)ptr);
	  // Set TLM response
	  trans.set_response_status(tlm::TLM_OK_RESPONSE);

        } else {
                    
	  v::error << name() << "Address not valid for write with ASI 0x19 (or read-only)" << v::endl;

	  // Setting TLM response
	  trans.set_response_status(tlm::TLM_ADDRESS_ERROR_RESPONSE);
        }

      } else {

	v::error << name() << "Unvalid TLM Command" << v::endl;
	// Set TLM response
	trans.set_response_status(tlm::TLM_COMMAND_ERROR_RESPONSE);

      }

    } else {

       v::error << name() << "Access to MMU registers, but MMU not present!" << v::endl;
       // Setting TLM response
       trans.set_response_status(tlm::TLM_ADDRESS_ERROR_RESPONSE);

    }

  // Ordinary access
  } else if ((asi == 0x8) || (asi == 0x9) || (asi == 0xa) || (asi == 0xb)) {

    // Read from memory/cache
    if (cmd == tlm::TLM_READ_COMMAND) {

      // Instruction scratchpad enabled && address points into selected 16 MB region
      if (m_ilram && (((addr >> 24) & 0xff) == m_ilramstart)) {

	ilocalram->mem_read((unsigned int)addr, ptr, len, &delay, debug, is_dbg);
	// Set TLM response
	trans.set_response_status(tlm::TLM_OK_RESPONSE);

      // Data scratchpad enabled && address points into selected 16MB region
      } else if (m_dlram && (((addr >> 24) & 0xff) == m_dlramstart)) {

	dlocalram->mem_read((unsigned int)addr, ptr, len, &delay, debug, is_dbg);
	// Set TLM response
	trans.set_response_status(tlm::TLM_OK_RESPONSE);

      // Cache access || bypass || direct mmu
      } else {

        dcache->mem_read((unsigned int)addr, ptr, len, &delay, debug, is_dbg);
	// Set TLM response
	trans.set_response_status(tlm::TLM_OK_RESPONSE);

      }

    // Write to memory/cache
    } else if (cmd == tlm::TLM_WRITE_COMMAND) {

      // Instruction scratchpad enabled && address points into selected 16 MB region
      if (m_ilram && (((addr >> 24) & 0xff) == m_ilramstart)) {

	ilocalram->mem_write((unsigned int)addr, ptr, len, &delay, debug, is_dbg);
	// set TLM response
	trans.set_response_status(tlm::TLM_OK_RESPONSE);

      // Data scratchpad enabled && address points into selected 16MB region
      } else if (m_dlram && (((addr >> 24) & 0xff) == m_dlramstart)) {

        dlocalram->mem_write((unsigned int)addr, ptr, len, &delay, debug, is_dbg);
	// Set TLM response
	trans.set_response_status(tlm::TLM_OK_RESPONSE);

      // Cache access (write through) || bypass || direct mmu
      } else {

        dcache->mem_write((unsigned int)addr, ptr, len, &delay, debug, is_dbg);
	// Set TLM response
	trans.set_response_status(tlm::TLM_OK_RESPONSE);

      }
      
    } else {

      v::error << name() << "TLM command not valid" << v::endl;
      // Setting TLM response
      trans.set_response_status(tlm::TLM_COMMAND_ERROR_RESPONSE);

    }

  } else {

    v::error << name() << "ASI not recognized: " << hex << asi << v::endl;
    // Setting TLM response
    trans.set_response_status(tlm::TLM_ADDRESS_ERROR_RESPONSE);
    
  }
}

/// TLM blocking forward transport function for icio socket
void mmu_cache::icio_b_transport(tlm::tlm_generic_payload& trans, sc_core::sc_time& delay) {

  // Call the functional part of the model
  // ---------------------------
  exec_instr(trans, delay, false);
  // ---------------------------

  // Consume component delay
  wait(delay);

  // Reset delay
  delay = SC_ZERO_TIME;

}

/// TLM forward blocking transport function for dcio socket
void mmu_cache::dcio_b_transport(tlm::tlm_generic_payload& trans, sc_core::sc_time& delay) {

  // Call the functional part of the model
  // -----------------------
  exec_data(trans, delay, false);
  // -----------------------

  // Consume component delay
  wait(delay);

  // Reset delay
  delay = SC_ZERO_TIME;
   
}

/// TLM non-blocking forward transport function for icio socket
tlm::tlm_sync_enum mmu_cache::icio_nb_transport_fw(tlm::tlm_generic_payload &trans, tlm::tlm_phase &phase, sc_core::sc_time &delay) {

  v::debug << name() << "ICIO nb_transport forward received transaction: " << hex << &trans << " with phase " << phase << v::endl;

  // The master has sent BEGIN_REQ
  if (phase == tlm::BEGIN_REQ) {

    // Put transaction in PEQ
    icio_PEQ.notify(trans, delay);

    // Advance transaction state.
    phase = tlm::END_REQ;

    // Return state
    return tlm::TLM_UPDATED;

  } else if (phase == tlm::END_RESP) {

    // Return state
    return tlm::TLM_COMPLETED;

  } else {

    v::error << name() << "Illegal phase in call to icio_nb_transport_fw!" << v::endl;
    trans.set_response_status(tlm::TLM_COMMAND_ERROR_RESPONSE);

  }

  return tlm::TLM_COMPLETED;
}

/// Processes transactions from the icio_PEQ.
/// Contains a state machine to manage the communication path back to instruction initiator
/// Is registered as an SC_THREAD and sensitive to icio_PEQ.get_event() 
void mmu_cache::icio_service_thread() {

  tlm::tlm_generic_payload *trans;
  tlm::tlm_sync_enum status;

  sc_core::sc_time delay;

  while(1) {

    // Process all icio transactions scheduled for the current time.
    // A return value of NULL indicates that the PEQ is empty at this time.

    while((trans = icio_PEQ.get_next_transaction()) != 0) {

      v::debug << name() << "Start ICIO transaction response processing." << v::endl;

      delay = SC_ZERO_TIME;

      // Call the functional part of the model
      // -------------------------------------
      exec_instr(*trans, delay, false);
      // -------------------------------------

      v::debug << name() << "Consume component delay: " << delay << v::endl;

      // Consume delay
      wait(delay);
      delay = SC_ZERO_TIME;

      // Call master with phase BEGIN_RESP
      tlm::tlm_phase phase = tlm::BEGIN_RESP;

      v::debug << name() << "Call icio backward transport with phase " << phase << v::endl;
      status = icio->nb_transport_bw(*trans, phase, delay);

      // Check return status
      switch (status) {

	case tlm::TLM_COMPLETED:

	  wait(delay);
	  break;

	case tlm::TLM_ACCEPTED:

	  wait(delay);
	  break;

	default:

	  v::error << name() << "TLM return status undefined or not valid!! " << v::endl;
	  assert(0);

      } // switch

    } // while PEQ
  
    wait();

  }  // while thread
}

/// TLM non-blocking forward transport function for dcio socket
tlm::tlm_sync_enum mmu_cache::dcio_nb_transport_fw(tlm::tlm_generic_payload &trans, tlm::tlm_phase &phase, sc_core::sc_time &delay) {

  v::debug << name() << "DCIO nb_transport forward received transaction: " << hex << &trans << " with  phase " << phase << v::endl;

  // The master has sent BEGIN_REQ
  if (phase == tlm::BEGIN_REQ) {

    // Put transaction in PEQ
    dcio_PEQ.notify(trans, delay);

    // Advance transaction state
    phase = tlm::END_REQ;

    // Return state
    return tlm::TLM_UPDATED;

  } else if (phase == tlm::END_RESP) {

    // Return state
    return tlm::TLM_COMPLETED;

  } else {
   
    v::error << name() << " Illegal phase in call to dcio_nb_transport_fw !" << v::endl;
    trans.set_response_status(tlm::TLM_COMMAND_ERROR_RESPONSE);

  }

  return tlm::TLM_COMPLETED;

}

/// Processes transactions from the dcio_PEQ.
/// Contains a state machine to manage the communication path back to the data initiator
/// Is registered as an SC_THREAD and sensitive to dcio_PEQ.get_event()
void mmu_cache::dcio_service_thread() {

  tlm::tlm_generic_payload *trans;
  tlm::tlm_sync_enum status;

  sc_core::sc_time delay;

  while(1) {

    // Process all dcio transactions scheduled for the current time.
    // A return value of NULL indicates that the PEQ is empty at this time.

    while((trans = dcio_PEQ.get_next_transaction()) != 0) {

      v::debug << name() << "Start dcio transaction response processing." << v::endl;

      delay = SC_ZERO_TIME;

      // Call the functional part of the model
      // -------------------------------------
      exec_data(*trans, delay, false);
      // -------------------------------------

      v::debug << name() << "Consume component delay: " << delay << v::endl;

      // Consume delay
      wait(delay);
      delay = SC_ZERO_TIME;

      // Call master with phase BEGIN_RESP
      tlm::tlm_phase phase = tlm::BEGIN_RESP;

      v::debug << name() << "Call to dcio backward transport with phase " << phase << v::endl;
      status = dcio->nb_transport_bw(*trans, phase, delay);

      // Check return status
      switch (status) {

        case tlm::TLM_COMPLETED:

	  wait(delay);
	  break;

        case tlm::TLM_ACCEPTED:

	  wait(delay);
	  break;

        default:

	  v::error << name() << "TLM return status undefined or not valid!! " << v::endl;
	  break;
 
      } // switch
    
    } // while PEQ

    wait();

  } // while thread     
}

// Instruction debug transport
unsigned int mmu_cache::icio_transport_dbg(tlm::tlm_generic_payload &trans) {

  sc_core::sc_time zero_delay = SC_ZERO_TIME;

  // Call the functional part of the model (in debug mode)
  // ----------------------------------
  exec_instr(trans, zero_delay, true);
  // -----------------------------------

  return trans.get_data_length();

}

// Data debug transport
unsigned int mmu_cache::dcio_transport_dbg(tlm::tlm_generic_payload &trans) {

  sc_core::sc_time zero_delay = SC_ZERO_TIME;

  // Call the functional part of the model (in debug mode)
  // ---------------------------------
  exec_data(trans, zero_delay, true);
  // ---------------------------------

  return trans.get_data_length();

} 

// Delayed release of transactions (for non-blocking pipelined transactions)
void mmu_cache::cleanUP() {

  tlm::tlm_generic_payload * trans;

  while(1) {

    wait(mEndTransactionPEQ.get_event());

    while((trans = mEndTransactionPEQ.get_next_transaction())) {

      v::debug << name() << "Release transaction: " << hex << trans << v::endl;

      ahb.release_transaction(trans);

    }
  }
}

// TLM non-blocking backward transport function for ahb socket
tlm::tlm_sync_enum mmu_cache::ahb_nb_transport_bw(tlm::tlm_generic_payload &trans, tlm::tlm_phase &phase, sc_core::sc_time &delay) {

  v::debug << name() << "nb_transport_bw received transaction " << hex << &trans << " with phase " << phase << v::endl;

  // The slave has sent END_REQ
  if (phase == tlm::END_REQ) {

    // In case END_REQ comes via backward path:
    // Notify interface functions that request phase is over.
    mEndRequestEvent.notify();

    // Slave is ready for BEGIN_DATA (writes only)
    if (trans.get_command() == tlm::TLM_WRITE_COMMAND) {

      // Put into DataPEQ for data phase processing
      mDataPEQ.notify(trans);

    }

  // New response (read operations only)
  } else if (phase == tlm::BEGIN_RESP) {

    mEndRequestEvent.notify();

    // Put into ResponsePEQ for response processing
    mResponsePEQ.notify(trans, delay);

  // Data phase completed
  } else if (phase == amba::END_DATA) {

    // Release transaction
    mEndTransactionPEQ.notify(trans, delay);

  // Phase not valid
  } else {

    v::error << name() << "Invalid phase in call to nb_transport_bw!" << v::endl;
    trans.set_response_status(tlm::TLM_COMMAND_ERROR_RESPONSE);

  }

  return tlm::TLM_ACCEPTED;

}


/// Function for write access to AHB master socket
void mmu_cache::mem_write(unsigned int addr, unsigned char * data,
                          unsigned int length, sc_core::sc_time * t,
                          unsigned int * debug, bool is_dbg) {

    tlm::tlm_phase phase;
    tlm::tlm_sync_enum status;
    sc_core::sc_time delay;

    // Allocate new transaction
    tlm::tlm_generic_payload *trans = ahb.get_transaction();

    v::debug << name() << "Allocate new transaction " << hex << trans << v::endl;

    // Initialize transaction
    trans->set_command(tlm::TLM_WRITE_COMMAND);
    trans->set_address(addr);
    trans->set_data_length(length);
    trans->set_data_ptr(data);
    trans->set_response_status(tlm::TLM_INCOMPLETE_RESPONSE);

    // Set burst size extension
    amba::amba_burst_size* size_ext;
    ahb.validate_extension<amba::amba_burst_size> (*trans);
    ahb.get_extension<amba::amba_burst_size> (size_ext, *trans);
    size_ext->value = (length < 4)? length : 4;

    // Set master id extension
    amba::amba_id* m_id;
    ahb.validate_extension<amba::amba_id> (*trans);
    ahb.get_extension<amba::amba_id> (m_id, *trans);
    m_id->value = m_master_id;
 
    // Set transfer type extension
    amba::amba_trans_type * trans_ext;
    ahb.validate_extension<amba::amba_trans_type>(*trans);
    ahb.get_extension<amba::amba_trans_type> (trans_ext, *trans);
    trans_ext->value = amba::NON_SEQUENTIAL;
    
    // Initialize delay
    delay = SC_ZERO_TIME;

    // Timed transport
    if (!is_dbg) {

      if (m_abstractionLayer == amba::amba_LT) {

        // Blocking transport
        ahb->b_transport(*trans, delay);

        // Consume delay
        wait(delay);
        delay = SC_ZERO_TIME;

      } else {

        // Initial phase for AT
        phase = tlm::BEGIN_REQ;

        v::debug << name() << "Transaction " << hex << trans << " call to nb_transport_fw with phase " << phase << v::endl;

        // Non-blocking transport
        status = ahb->nb_transport_fw(*trans, phase, delay);

        switch (status) {

          case tlm::TLM_ACCEPTED:
          case tlm::TLM_UPDATED:

	    if (phase == tlm::BEGIN_REQ) {

	      // The slave returned TLM_ACCEPTED.
	      // Wait until END_REQ comes in on backward path
	      // before starting DATA phase.
	      wait(mEndRequestEvent);

	    } else if (phase == tlm::END_REQ) {

	      // The slave returned TLM_UPDATED with END_REQ
	      mDataPEQ.notify(*trans, delay);

	    } else if (phase == amba::END_DATA) {

	      // Done return control to user.

	    } else {

	      // Forbidden phase
	      v::error << name() << "Invalid phase in return path (from call to nb_transport_fw)!" << v::endl;
	      trans->set_response_status(tlm::TLM_COMMAND_ERROR_RESPONSE);

	    }

	    break;

            case tlm::TLM_COMPLETED:

	      // Slave directly jumps to TLM_COMPLETED (Pseudo AT).
	      // Don't send END_RESP
	      // wait(delay)
	  
	      break;

            default:

	      v::error << name() << "Invalid return value from call to nb_transport_fw!" << v::endl;
	      trans->set_response_status(tlm::TLM_COMMAND_ERROR_RESPONSE);

	}
      }

    } else {

      // Debug transport
      ahb->transport_dbg(*trans);

    }
}

/// Function for read access to AHB master socket
bool mmu_cache::mem_read(unsigned int addr, unsigned char * data,
                         unsigned int length, sc_core::sc_time * t,
                         unsigned int * debug, bool is_dbg) {

    tlm::tlm_phase phase;
    tlm::tlm_sync_enum status;
    sc_core::sc_time delay;

    bool cacheable;

    // Allocate new transaction
    tlm::tlm_generic_payload *trans = ahb.get_transaction();

    v::debug << name() << "Allocate new transaction: " << hex << trans << v::endl;

    // Initialize transaction
    trans->set_command(tlm::TLM_READ_COMMAND);
    trans->set_address(addr);
    trans->set_data_length(length);
    trans->set_data_ptr(data);
    trans->set_response_status(tlm::TLM_INCOMPLETE_RESPONSE);

    // Set burst size extension
    amba::amba_burst_size* size_ext;
    ahb.validate_extension<amba::amba_burst_size> (*trans);
    ahb.get_extension<amba::amba_burst_size> (size_ext, *trans);
    size_ext->value = (length < 4)? length : 4;

    // Set master id extension
    amba::amba_id* m_id;
    ahb.validate_extension<amba::amba_id> (*trans);
    ahb.get_extension<amba::amba_id> (m_id, *trans);
    m_id->value = m_master_id;

    // Set transfer type extension
    amba::amba_trans_type * trans_ext;
    ahb.validate_extension<amba::amba_trans_type> (*trans);
    ahb.get_extension<amba::amba_trans_type> (trans_ext, *trans);
    
    // Init delay
    delay = SC_ZERO_TIME;

    // Timed transport
    if (!is_dbg) {

      if (m_abstractionLayer == amba::amba_LT) {

	// Blocking transport
	ahb->b_transport(*trans, delay);

	// Consume delay
	wait(delay);
	delay = SC_ZERO_TIME;

      } else {

	// Initial phase for AT
	phase = tlm::BEGIN_REQ;
      
	v::debug << name() << "Transaction " << hex << trans << " call to nb_transport_fw with phase " << phase << v::endl;

	// Non-blocking transport
	status = ahb->nb_transport_fw(*trans, phase, delay);

	switch(status) {

          case tlm::TLM_ACCEPTED:
          case tlm::TLM_UPDATED:

	    if (phase == tlm::BEGIN_REQ) {

	      // The slave returned TLM_ACCEPTED.
	      // Wait until BEGIN_RESP before giving control
	      // to the user (for sending next transaction).
	      wait(mEndRequestEvent);

	    } else if (phase == tlm::END_REQ) {

	      // The slave returned TLM_UPDATED with END_REQ

	      wait(mEndRequestEvent);

	    } else if (phase == tlm::BEGIN_RESP) {

	      // Slave directly jumped to BEGIN_RESP
	      // Notify the response thread and return control to user
	      mResponsePEQ.notify(*trans, delay);

	    } else {

	      // Forbidden phase
	      v::error << name() << "Invalid phase in return path (from call to nb_transport_fw)!" << v::endl;
	      trans->set_response_status(tlm::TLM_COMMAND_ERROR_RESPONSE);

	    }

	    break;

	  case tlm::TLM_COMPLETED:
	
	    // Slave directly jumps to TLM_COMPLETED (Pseudo AT).
	    // Don't send END_RESP
	    // wait(delay)

	    break;

          default:

	    v::error << name() << "Invalid return value from call to nb_transport_fw!" << v::endl;
	    trans->set_response_status(tlm::TLM_COMMAND_ERROR_RESPONSE);

	}

	// Wait for result to be ready before return
	//wait(mResponsePEQ.get_event());

      }

    } else {

      // Debug transport
      ahb->transport_dbg(*trans);

    }

    // Check cacheability:
    // -------------------
    // Cacheable areas are defined by the 'cached' parameter.
    // In case 'cached' is zero, plug & play information will be used.
    if (m_cached != 0) {

	// use fixed mask
	cacheable = (m_cached & (1 << (addr >> 28))) ? true : false;

    } else {

	// use PNP - information is carried by protection extension
	cacheable = (ahb.get_extension<amba::amba_cacheable>(*trans)) ? true : false;
	
    }    

    // return cacheability state
    return cacheable;
}

// Thread for data phase processing in write operations (sends BEGIN_DATA)
void mmu_cache::DataThread() {

  tlm::tlm_generic_payload* trans;
  tlm::tlm_phase phase;
  sc_core::sc_time delay;
  tlm::tlm_sync_enum status;

  while(1) {

    // v::debug << name() << "Data thread waiting for new data phase." << v::endl;

    // Wait for new data phase
    wait(mDataPEQ.get_event());

    // v::debug << name() << "DataPEQ Event" << v::endl;

    // Get transaction from PEQ
    trans = mDataPEQ.get_next_transaction();

    // Prepare BEGIN_DATA
    phase = amba::BEGIN_DATA;
    delay = SC_ZERO_TIME;

    v::debug << name() << "Transaction " << hex << trans << " call to nb_transport_fw with phase " << phase << v::endl;

    // Call to nb_transport_fw with BEGIN_DATA
    status = ahb->nb_transport_fw(*trans, phase, delay);

    switch(status) {
      
      case tlm::TLM_ACCEPTED:
      case tlm::TLM_UPDATED:

	if (phase == amba::BEGIN_DATA) {

	  // The slave returned TLM_ACCEPTED.
	  // Wait for END_DATA to come in on backward path.

	  // v::debug << name() << "Waiting mEndDataEvent" << v::endl;
	  //wait(mEndDataEvent);
	  // v::debug << name() << "mEndDataEvent" << v::endl;

        } else if (phase == amba::END_DATA) {

	  // Slave returned TLM_UPDATED with END_DATA
	  // Data phase completed.
	  wait(delay);

	} else {

	  // Forbidden phase
	  v::error << name() << "Invalid phase in return path (from call to nb_transport_fw)!" << v::endl;

	}
	
	break;

      case tlm::TLM_COMPLETED:

	// Slave directly jumps to TLM_COMPLETED (Pseudo AT).
	// wait(delay);

	break;

    }
  }
}


// Thread for response synchronization (sync and send END_RESP)
void mmu_cache::ResponseThread() {

  tlm::tlm_generic_payload* trans;
  tlm::tlm_phase phase;
  sc_core::sc_time delay;
  tlm::tlm_sync_enum status;

  while(1) {

    // Wait for response from slave
    wait(mResponsePEQ.get_event());

    // Get transaction from PEQ
    trans = mResponsePEQ.get_next_transaction();

    // Prepare END_RESP
    phase = tlm::END_RESP;
    delay = sc_core::SC_ZERO_TIME;

    v::debug << name() << "Transaction " << hex << trans << " call to nb_transport_fw with phase " << phase << v::endl;

    // Call to nb_transport_fw
    status = ahb->nb_transport_fw(*trans, phase, delay);

    // Return value must be TLM_COMPLETED or TLM_ACCEPTED
    assert((status==tlm::TLM_COMPLETED)||(status==tlm::TLM_ACCEPTED));

    // Cleanup
    mEndTransactionPEQ.notify(*trans, delay);

  }
}



/// writes the cache control register and handles the commands
void mmu_cache::write_ccr(unsigned char * data, unsigned int len,
                          sc_core::sc_time * delay, bool is_dbg) {

    unsigned int tmp1 = *(unsigned int *)data;
    unsigned int tmp;

    unsigned int dummy;

    #ifdef LITTLE_ENDIAN_BO
    swap_Endianess(tmp1);
    #endif

    memcpy(&tmp, &tmp1, len);

    // [DS] data cache snoop enable (todo)
    if (tmp & (1 << 23)) {
    }
    // [FD] dcache flush (do not set; always reads as zero)
    if (tmp & (1 << 22)) {
        dcache->flush(delay, &dummy, is_dbg);
    }
    // [FI] icache flush (do not set; always reads as zero)
    if (tmp & (1 << 21)) {
        icache->flush(delay, &dummy, is_dbg);
    }
    // [IB] instruction burst fetch (todo)
    if (tmp & (1 << 16)) {
    }

    // [IP] Instruction cache flush pending (bit 15 - read only)
    // [DP] Data cache flush pending (bit 14 - read only)

    // [DF] data cache freeze on interrupt (todo)
    if (tmp & (1 << 5)) {
    }

    // [IF] instruction cache freeze on interrupt (todo)
    if (tmp & (1 << 4)) {
    }

    // [DCS] data cache state (bits 3:2 - read only)
    // [ICS] instruction cache state (bits 1:0 - read only)

    // read only masking: 1111 1111 1001 1111 0011 1111 1111 1111
    CACHE_CONTROL_REG = (tmp & 0xff9f3fff);

    v::debug << name() << "CACHE_CONTROL_REG: " << hex << v::setw(8) << CACHE_CONTROL_REG << v::endl;
}

// read the cache control register
unsigned int mmu_cache::read_ccr() {

  unsigned int tmp = CACHE_CONTROL_REG;

  #ifdef LITTLE_ENDIAN_BO
  swap_Endianess(tmp);
  #endif

  return (tmp);
}

// Snooping function
void mmu_cache::snoopingCallBack(const t_snoop& snoop, const sc_core::sc_time& delay) {

  v::debug << name() << "Snooping write operation on AHB interface (MASTER: " << snoop.master_id << " ADDR: " \
	   << hex << snoop.address << " LENGTH: " << snoop.length << ")" << v::endl;

  // Make sure we are not snooping ourself ;)
  if (snoop.master_id != m_master_id) {

    // If dcache and snooping enabled
    if (m_dcen && m_dsnoop) {

      dcache->snoop_invalidate(snoop, delay);

    }
  }
}

// Helper for setting clock cycle latency using sc_clock argument
void mmu_cache::clk(sc_core::sc_clock &clk) {

  // Set local clock
  clockcycle = clk.period();

  // Set icache clock
  if (m_icen) { 
    
    icache->clk(clk);

  }

  // Set dcache clock
  if (m_dcen) {

    dcache->clk(clk);

  }

  // Set mmu clock
  if (m_mmu_en) {

    m_mmu->clk(clk);

  }

}

// Helper for setting clock cycle latency using sc_time argument
void mmu_cache::clk(sc_core::sc_time &period) {

  // Set local clock
  clockcycle = period;

  // Set icache clock
  if (m_icen) { 
    
    icache->clk(period);

  }

  // Set dcache clock
  if (m_dcen) {

    dcache->clk(period);

  }

  // Set mmu clock
  if (m_mmu_en) {

    m_mmu->clk(period);

  }

}

// Helper for setting clock cycle latency using a value-time_unit pair
void mmu_cache::clk(double period, sc_core::sc_time_unit base) {

  // Set local clock
  clockcycle = sc_core::sc_time(period, base);

  // Set icache clock
  if (m_icen) { 
    
    icache->clk(period, base);

  }

  // Set dcache clock
  if (m_dcen) {

    dcache->clk(period, base);

  }

  // Set mmu clock
  if (m_mmu_en) {

    m_mmu->clk(period, base);

  }

}
