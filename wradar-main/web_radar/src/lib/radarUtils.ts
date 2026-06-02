export function worldToRadar(
    worldX: number,
    worldY: number,
    posX: number,
    posY: number,
    scale: number,
    currentWidth: number,
    currentHeight: number,
    originalWidth: number,
    originalHeight: number
): { x: number; y: number } {
    const px = (worldX - posX) / scale;
    const py = (posY - worldY) / scale;
    const proportionalX = px / originalWidth;
    const proportionalY = py / originalHeight;
    const finalX = proportionalX * currentWidth;
    const finalY = proportionalY * currentHeight;
    const clampedX = Math.max(0, Math.min(currentWidth, finalX));
    const clampedY = Math.max(0, Math.min(currentHeight, finalY));
    return { x: clampedX, y: clampedY };
}

export function getWeaponIconPath(weaponName: string): string {
    return "weapons/" + weaponName.toLocaleLowerCase() + ".svg";
}

export const MAP_WIDTH = 1024;
export const MAP_HEIGHT = 1024;
