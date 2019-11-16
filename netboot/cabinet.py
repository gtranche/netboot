import ipaddress
import os.path
import threading
import time
import yaml
from typing import Dict, List, Optional, Sequence, Tuple, Union

from netboot.hostutils import Host


class CabinetException(Exception):
    pass


class Cabinet:
    STATE_STARTUP = "startup"
    STATE_WAIT_FOR_CABINET_POWER_ON = "wait_power_on"
    STATE_SEND_CURRENT_GAME = "send_game"
    STATE_WAIT_FOR_CABINET_POWER_OFF = "wait_power_off"

    REGION_UNKNOWN = "japan"
    REGION_JAPAN = "japan"
    REGION_USA = "usa"
    REGION_EXPORT = "export"
    REGION_KOREA = "korea"
    REGION_AUSTRALIA = "australia"

    def __init__(
        self,
        ip: str,
        region: str,
        description: str,
        filename: Optional[str],
        patches: Dict[str, Sequence[str]],
        target: Optional[str] = None,
        version: Optional[str] = None
    ) -> None:
        if region not in [self.REGION_JAPAN, self.REGION_USA, self.REGION_EXPORT, self.REGION_KOREA, self.REGION_AUSTRALIA]:
            raise CabinetException(f"Unrecognized region {region}!")
        self.description: str = description
        self.region: str = region
        self.patches: Dict[str, List[str]] = {rom: [p for p in patches[rom]] for rom in patches}
        self.__host: Host = Host(ip, target=target, version=version)
        self.__lock: threading.Lock = threading.Lock()
        self.__current_filename: Optional[str] = filename
        self.__new_filename: Optional[str] = filename
        self.__state: Tuple[str, int] = (self.STATE_STARTUP, 0)

    def _clone_state(self, other_cabinet: "Cabinet") -> None:
        state: Optional[Tuple[str, int]] = None
        with other_cabinet.__lock:
            if other_cabinet.__state[0] != self.STATE_SEND_CURRENT_GAME:
                state = other_cabinet.__state
        if state is not None:
            with self.__lock:
                self.__state = state

    def __repr__(self) -> str:
        return f"Cabinet(ip={repr(self.ip)}, description={repr(self.description)}, filename={repr(self.filename)}, patches={repr(self.patches)} target={repr(self.target)}, version={repr(self.version)})"

    @property
    def ip(self) -> str:
        return self.__host.ip

    @property
    def target(self) -> str:
        return self.__host.target

    @property
    def version(self) -> str:
        return self.__host.version

    @property
    def filename(self) -> Optional[str]:
        with self.__lock:
            return self.__new_filename

    @filename.setter
    def filename(self, new_filename: Optional[str]) -> None:
        with self.__lock:
            self.__new_filename = new_filename

    def tick(self) -> None:
        """
        Tick the state machine forward.
        """

        with self.__lock:
            self.__host.tick()
            current_state = self.__state[0]

            # Startup state, only one transition to waiting for cabinet
            if current_state == self.STATE_STARTUP:
                self.__state = (self.STATE_WAIT_FOR_CABINET_POWER_ON, 0)
                return

            # Wait for cabinet to power on state, transition to sending game
            # if the cabinet is active, transition to self if cabinet is not.
            if current_state == self.STATE_WAIT_FOR_CABINET_POWER_ON:
                if self.__host.alive:
                    if self.__new_filename is None:
                        # Skip sending game, there's nothing to send
                        self.__state = (self.STATE_WAIT_FOR_CABINET_POWER_OFF, 0)
                    else:
                        self.__host.send(self.__new_filename, self.patches.get(self.__new_filename, []))
                        self.__state = (self.STATE_SEND_CURRENT_GAME, 0)
                return

            # Wait for send to complete state. Transition to waiting for
            # cabinet power on if transfer failed. Stay in state if transfer
            # continuing. Transition to waint for power off if transfer success.
            if current_state == self.STATE_SEND_CURRENT_GAME:
                if self.__host.status == Host.STATUS_INACTIVE:
                    raise Exception("State error, shouldn't be possible!")
                elif self.__host.status == Host.STATUS_TRANSFERRING:
                    current, total = self.__host.progress
                    self.__state = (self.STATE_SEND_CURRENT_GAME, int(float(current * 100) / float(total)))
                elif self.__host.status == Host.STATUS_FAILED:
                    self.__state = (self.STATE_WAIT_FOR_CABINET_POWER_ON, 0)
                elif self.__host.status == Host.STATUS_COMPLETED:
                    self.__host.reboot()
                    self.__state = (self.STATE_WAIT_FOR_CABINET_POWER_OFF, 0)
                return

            # Wait for cabinet to turn off again. Transition to waiting for
            # power to come on if the cabinet is inactive. Transition to
            # waiting for power to come on if game changes. Stay in state
            # if cabinet stays on.
            if current_state == self.STATE_WAIT_FOR_CABINET_POWER_OFF:
                if not self.__host.alive:
                    self.__state = (self.STATE_WAIT_FOR_CABINET_POWER_ON, 0)
                elif self.__current_filename != self.__new_filename:
                    self.__current_filename = self.__new_filename
                    self.__state = (self.STATE_WAIT_FOR_CABINET_POWER_ON, 0)
                return

            raise Exception("State error, impossible state!")

    @property
    def state(self) -> Tuple[str, int]:
        """
        Returns the current state as a string, and the progress through that state
        as an integer, bounded between 0-100.
        """
        with self.__lock:
            return self.__state


class CabinetManager:
    def __init__(self, cabinets: Sequence[Cabinet]) -> None:
        self.__cabinets: Dict[str, Cabinet] = {cab.ip: cab for cab in cabinets}
        self.__lock: threading.Lock = threading.Lock()
        self.__thread: threading.Thread = threading.Thread(target=self.__poll_thread)
        self.__thread.setDaemon(True)
        self.__thread.start()

    def __repr__(self) -> str:
        return f"CabinetManager([{', '.join(repr(cab) for cab in self.cabinets)}])"

    @staticmethod
    def from_yaml(yaml_file: str) -> "CabinetManager":
        with open(yaml_file, "r") as fp:
            data = yaml.safe_load(fp)

        if data is None:
            # Assume this is an empty file
            return CabinetManager([])

        if not isinstance(data, dict):
            raise CabinetException(f"Invalid YAML file format for {yaml_file}, missing list of cabinets!")

        cabinets: List[Cabinet] = []
        for ip, cab in data.items():
            try:
                ip = str(ipaddress.IPv4Address(ip))
            except ValueError:
                raise CabinetException("Invalid YAML file format for {yaml_file}, IP address {ip} is not valid!")

            if not isinstance(cab, dict):
                raise CabinetException(f"Invalid YAML file format for {yaml_file}, missing cabinet details for {ip}!")
            for key in ["description", "filename", "patches"]:
                if key not in cab:
                    raise CabinetException(f"Invalid YAML file format for {yaml_file}, missing {key} for {ip}!")
            if cab['filename'] is not None and not os.path.isfile(str(cab['filename'])):
                raise CabinetException(f"Invalid YAML file format for {yaml_file}, file {cab['filename']} for {ip} is not a file!")
            for rom, patches in cab['patches'].items():
                if not os.path.isfile(str(rom)):
                    raise CabinetException(f"Invalid YAML file format for {yaml_file}, file {rom} for {ip} is not a file!")
                for patch in patches:
                    if not os.path.isfile(str(patch)):
                        raise CabinetException(f"Invalid YAML file format for {yaml_file}, file {patch} for {ip} is not a file!")

            cabinets.append(
                Cabinet(
                    ip=ip,
                    description=str(cab['description']),
                    region=str(cab['region']).lower(),
                    filename=str(cab['filename']) if cab['filename'] is not None else None,
                    patches={str(rom): [str(p) for p in cab['patches'][rom]] for rom in cab['patches']},
                    target=str(cab['target']) if 'target' in cab else None,
                    version=str(cab['version']) if 'version' in cab else None,
                )
            )

        return CabinetManager(cabinets)

    def to_yaml(self, yaml_file: str) -> None:
        data: Dict[str, Dict[str, Optional[Union[str, Dict[str, List[str]]]]]] = {}

        with self.__lock:
            cabinets: List[Cabinet] = sorted([cab for _, cab in self.__cabinets.items()], key=lambda cab: cab.ip)

        for cab in cabinets:
            data[cab.ip] = {
                'description': cab.description,
                'region': cab.region,
                'target': cab.target,
                'version': cab.version,
                'filename': cab.filename,
                'patches': cab.patches,
            }

        with open(yaml_file, "w") as fp:
            yaml.dump(data, fp)

    def __poll_thread(self) -> None:
        while True:
            with self.__lock:
                cabinets: List[Cabinet] = [cab for _, cab in self.__cabinets.items()]

            for cabinet in cabinets:
                cabinet.tick()

            time.sleep(1)

    @property
    def cabinets(self) -> List[Cabinet]:
        with self.__lock:
            return sorted([cab for _, cab in self.__cabinets.items()], key=lambda cab: cab.ip)

    def cabinet(self, ip: str) -> Cabinet:
        with self.__lock:
            if ip not in self.__cabinets:
                raise CabinetException(f"There is no cabinet with the IP {ip}")
            return self.__cabinets[ip]

    def add_cabinet(self, cab: Cabinet) -> None:
        with self.__lock:
            if cab.ip in self.__cabinets:
                raise CabinetException(f"There is already a cabinet with the IP {cab.ip}")
            self.__cabinets[cab.ip] = cab

    def remove_cabinet(self, ip: str) -> None:
        with self.__lock:
            if ip not in self.__cabinets:
                raise CabinetException(f"There is no cabinet with the IP {ip}")
            del self.__cabinets[ip]

    def update_cabinet(self, cab: Cabinet) -> None:
        with self.__lock:
            ip = cab.ip
            if ip not in self.__cabinets:
                raise CabinetException(f"There is no cabinet with the IP {ip}")
            # Make sure we don't reboot the cabinet if we update settings.
            cab._clone_state(self.__cabinets[ip])
            self.__cabinets[ip] = cab

    def cabinet_exists(self, ip: str) -> bool:
        with self.__lock:
            return ip in self.__cabinets
