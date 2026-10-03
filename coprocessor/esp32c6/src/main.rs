//! Freya's ESP32-C6 network coprocessor.  The C6 owns Wi-Fi, DHCP, DNS,
//! ICMP, TCP/UDP and TLS; Freya talks to it with fixed 512-byte RPC frames
//! over SPI, the C6 being the slave.
//!
//! Each command transaction is followed by a FETCH transaction that clocks
//! out the response.  READY goes high once a response (or an event) has
//! been queued for that FETCH.  The last response is kept by sequence
//! number, so a retried command does not repeat its side effect.

mod dispatch;
mod http;
mod os;
mod ping;
mod term;
mod web;

use esp_idf_svc::eventloop::EspSystemEventLoop;
use esp_idf_svc::hal::peripherals::Peripherals;
use esp_idf_svc::netif::IpEvent;
use esp_idf_svc::nvs::EspDefaultNvsPartition;
use esp_idf_svc::sys::{self, esp, EspError};
use esp_idf_svc::wifi::{EspWifi, WifiEvent};
use freya_c6::frame::{self, Frame, EVENT, PAYLOAD, SIZE};
use log::info;

use dispatch::{Coproc, FETCH};

/// The bench-tested wiring; keep the README and docs/network.md in step.
const PIN_MOSI: i32 = 7;
const PIN_MISO: i32 = 2;
const PIN_SCLK: i32 = 6;
const PIN_CS: i32 = 14;
const PIN_READY: i32 = 4;

const SPI_HOST: sys::spi_host_device_t = sys::spi_host_device_t_SPI2_HOST;

fn spi_init() -> Result<(), EspError> {
    unsafe {
        esp!(sys::gpio_set_direction(PIN_READY, sys::gpio_mode_t_GPIO_MODE_OUTPUT))?;
        esp!(sys::gpio_set_level(PIN_READY, 0))?;

        let mut bus: sys::spi_bus_config_t = core::mem::zeroed();
        bus.__bindgen_anon_1.mosi_io_num = PIN_MOSI;
        bus.__bindgen_anon_2.miso_io_num = PIN_MISO;
        bus.sclk_io_num = PIN_SCLK;
        bus.__bindgen_anon_3.quadwp_io_num = -1;
        bus.__bindgen_anon_4.quadhd_io_num = -1;
        bus.max_transfer_sz = SIZE as i32;
        let mut slave: sys::spi_slave_interface_config_t = core::mem::zeroed();
        slave.spics_io_num = PIN_CS;
        slave.queue_size = 1;
        slave.mode = 0;
        esp!(sys::spi_slave_initialize(SPI_HOST, &bus, &slave, sys::spi_common_dma_t_SPI_DMA_CH_AUTO))
    }
}

/// One full-frame transaction: `tx` goes out while `rx` comes in.  READY
/// is raised while it is queued if the master has something to fetch.
fn transfer(tx: &Frame, rx: &mut Frame, ready: bool) -> Result<(), EspError> {
    rx.fill(0);
    let mut t: sys::spi_slave_transaction_t = unsafe { core::mem::zeroed() };
    t.length = SIZE * 8;
    t.tx_buffer = tx.as_ptr().cast();
    t.rx_buffer = rx.as_mut_ptr().cast();
    unsafe {
        esp!(sys::spi_slave_queue_trans(SPI_HOST, &t, sys::TickType_t::MAX))?;
        if ready {
            sys::gpio_set_level(PIN_READY, 1);
        }
        let mut done: *mut sys::spi_slave_transaction_t = core::ptr::null_mut();
        let result = esp!(sys::spi_slave_get_trans_result(SPI_HOST, &mut done, sys::TickType_t::MAX));
        sys::gpio_set_level(PIN_READY, 0);
        result
    }
}

fn serve(mut co: Coproc) -> Result<core::convert::Infallible, EspError> {
    spi_init()?;
    let tx: &mut Frame = os::dma_buffer();
    let rx: &mut Frame = os::dma_buffer();
    let mut cached: Box<Frame> = Box::new([0; SIZE]);
    let mut cached_sequence: Option<u32> = None;
    let mut queued_reply = false;
    let mut event: Option<[u8; 2]> = None;

    loop {
        transfer(tx, rx, queued_reply || event.is_some())?;
        let Some(header) = frame::decode(rx) else {
            tx.fill(0);
            queued_reply = false;
            continue;
        };
        if header.opcode == FETCH {
            // This FETCH took the queued response.  A pending event goes
            // out on a later FETCH that finds nothing else queued.
            if queued_reply {
                tx.fill(0);
                queued_reply = false;
            } else if let Some(payload) = event.take() {
                frame::encode(tx, EVENT, header.sequence, 0, &payload);
                queued_reply = true;
            } else {
                tx.fill(0);
            }
            continue;
        }
        if cached_sequence == Some(header.sequence) {
            tx.copy_from_slice(&cached[..]);
        } else {
            let mut reply = [0u8; PAYLOAD];
            let data = &rx[frame::HEADER..frame::HEADER + header.length];
            let (status, length) = co.dispatch(header.opcode, data, &mut reply);
            frame::encode(tx, header.opcode, header.sequence, status, &reply[..length]);
            cached.copy_from_slice(&tx[..]);
            cached_sequence = Some(header.sequence);
        }
        queued_reply = true;
        if event.is_none() {
            event = co.event();
        }
    }
}

fn main() -> Result<(), EspError> {
    sys::link_patches();
    esp_idf_svc::log::EspLogger::initialize_default();

    let peripherals = Peripherals::take()?;
    let sysloop = EspSystemEventLoop::take()?;
    let nvs = EspDefaultNvsPartition::take()?;
    let wifi = EspWifi::new(peripherals.modem, sysloop.clone(), Some(nvs.clone()))?;
    esp!(unsafe { sys::esp_wifi_set_mode(sys::wifi_mode_t_WIFI_MODE_STA) })?;

    let _got_ip = sysloop.subscribe::<IpEvent, _>(|event| {
        if let IpEvent::DhcpIpAssigned(_) = event {
            os::set_net_up(true);
        }
    })?;
    let _lost = sysloop.subscribe::<WifiEvent, _>(|event| {
        if let WifiEvent::StaDisconnected(_) = event {
            os::set_net_up(false);
        }
    })?;

    info!("Freya network coprocessor ready");
    os::spawn(c"freya_term", 16384, 5, term::run).expect("terminal task");
    os::spawn(c"freya_web", 16384, 5, web::run).expect("web task");

    // The SPI service runs here, above the network tasks.
    unsafe { sys::vTaskPrioritySet(core::ptr::null_mut(), 8) };
    let err = serve(Coproc::new(wifi, nvs)).unwrap_err();
    panic!("SPI link failed: {err}");
}
