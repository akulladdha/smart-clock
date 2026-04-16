import serial
import time

PORT = "/dev/ttyAMA3"
BAUD = 9600

try:
    with serial.Serial(PORT, BAUD, timeout=1) as ser:
        print(f"✅ Opened {ser.name} at {BAUD} baud")

        test_msg = b"Hello UART3!\n"

        ser.write(test_msg)
        print(f"📤 Sent:     {test_msg}")

        time.sleep(0.1)  # Wait for data to loop back

        received = ser.read(len(test_msg))
        print(f"📥 Received: {received}")

        if received == test_msg:
            print("✅ Loopback test PASSED!")
        else:
            print("❌ Loopback test FAILED — check your jumper wire")

except serial.SerialException as e:
    print(f"❌ Could not open port: {e}")
